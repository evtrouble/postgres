#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "postmaster/postmaster.h"
#include "postmaster/pm_backend_comm.h"
#include "utils/wait_event_types.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "miscadmin.h"

extern char *connection_pool_socket_dir;
extern char *connection_pool_socket_file_prefix;

/*
 * Send a new client socket to an idle backend (for connection pool reuse)
 *
 * This function is called by postmaster when it wants to assign a new
 * connection to an idle backend from the pool.
 *
 * 'pmchild' is the PMChild structure for the idle backend.
 * 'client_sock' is the new client socket to be passed to the backend.
 *
 * Returns true if the socket was successfully sent, false otherwise.
 * If false is returned, postmaster should fall back to creating a new backend.
 */
bool
send_socket_to_backend(PMChild *pmchild, ClientSocket *client_sock)
{
    struct msghdr   msg = {0};
    struct iovec    iov;
    char            control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    ssize_t         sent;
    char            sock_path[MAXPGPATH];

    /* 参数校验 */
    if (!pmchild || !client_sock || pmchild->pid == 0)
        return false;

    /* 如果 control_fd 尚未建立，尝试连接到 backend 的控制 socket */
    if (pmchild->control_fd < 0)
    {
        int fd;
        struct sockaddr_un addr;

        snprintf(sock_path, sizeof(sock_path),
                 "%s/%s.%d", connection_pool_socket_dir, connection_pool_socket_file_prefix, (int)pmchild->pid);

        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0)
        {
            elog(WARNING, "could not create socket to connect to backend (pid=%d): %m", (int)pmchild->pid);
            return false;
        }

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

        if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
        {
            int save_errno = errno;
            closesocket(fd);
            elog(WARNING, "could not connect to backend control socket '%s' (pid=%d): %m",
                 sock_path, (int)pmchild->pid);
            /* 可选：如果 ENOENT，说明 backend 已退出或未就绪 */
            if (save_errno == ENOENT)
                elog(DEBUG2, "backend control socket does not exist (pid=%d)", (int)pmchild->pid);
            return false;
        }

        /* 连接成功，缓存 fd 供后续复用（可选优化） */
        pmchild->control_fd = fd;
    }

    /* 此时 pmchild->control_fd 应为有效连接 */
    Assert(pmchild->control_fd >= 0);

    /* 准备要发送的普通数据：客户端地址 */
    iov.iov_base = &client_sock->raddr;
    iov.iov_len  = sizeof(client_sock->raddr);

    /* 准备控制消息：传递 client socket fd */
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &client_sock->sock, sizeof(int));

    /* 发送：同时传递 fd + 客户端地址 */
    // elog(DEBUG1, "sending raddr size = %zu, fd = %d", 
    //  sizeof(client_sock->raddr), client_sock->sock);
    sent = sendmsg(pmchild->control_fd, &msg, 0);
    // elog(DEBUG1, "sendmsg returned %zd", sent);
    if (sent != sizeof(client_sock->raddr))
    {
        int save_errno = errno;
        elog(WARNING, "failed to send client socket to backend (pid=%d): %m", (int)pmchild->pid);

        /* 关闭并重置失效的连接 */
        closesocket(pmchild->control_fd);
        pmchild->control_fd = -1;

        /* 如果对端已关闭，可能是 backend 退出了 */
        if (save_errno == EPIPE || save_errno == ECONNRESET)
            elog(DEBUG2, "backend (pid=%d) control channel closed", (int)pmchild->pid);

        return false;
    }

    /* 唤醒 backend */
    SetLatch(pmchild->procLatch);

    elog(DEBUG2, "sent client socket (fd=%d) to backend (pid=%d)",
         (int)client_sock->sock, (int)pmchild->pid);

    return true;
}


/*
 * Receive a new client socket from postmaster (for connection pool reuse)
 *
 * This function is called by backend processes waiting in the connection pool.
 * Postmaster sends the new client socket via Unix domain socket before
 * waking up the backend with SetLatch().
 */
void
receive_socket_from_postmaster(ClientSocket *cs)
{
    struct msghdr   msg;
    struct iovec    iov;
    char            control[CMSG_SPACE(sizeof(int))];
    struct cmsghdr *cmsg;
    ssize_t         n;
    int             rc;

    Assert(cs != NULL);

    for (;;)
    {
        if (MyControlFd < 0)
        {
            rc = WaitLatchOrSocket(
                    MyLatch,
                    WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
                    MyListenFd,
                    -1,
                    WAIT_EVENT_CLIENT_READ
                );

            if (ProcDiePending)
            {
                ResetLatch(MyLatch);
                return;
            }

            if (rc & WL_LATCH_SET)
            {
                ResetLatch(MyLatch);
                CHECK_FOR_INTERRUPTS();
                if (ProcDiePending)
                    return;
            }

            if (rc & WL_EXIT_ON_PM_DEATH)
            {
                return;
            }
            else if (!(rc & WL_SOCKET_READABLE))
            {
                continue;
            }

            MyControlFd = accept(MyListenFd, NULL, NULL);
            if (MyControlFd < 0)
            {
                int save_errno = errno;

                if (save_errno == EINTR)
                    continue;
#ifdef EAGAIN
                if (save_errno == EAGAIN)
                    continue;
#endif
#ifdef EWOULDBLOCK
                if (save_errno == EWOULDBLOCK)
                    continue;
#endif

                elog(WARNING, "accept on control socket failed: %m");
                continue;
            }
        }

		cs->sock = PGINVALID_SOCKET;
        iov.iov_base = &cs->raddr;
        iov.iov_len = sizeof(cs->raddr);

        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);

        /*
         * Wait for data on control channel.
         * We use WaitLatchOrSocket to be interruptible by signals.
         */
        rc = WaitLatchOrSocket(
            MyLatch,
            WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
            MyControlFd,
            -1,  /* no timeout */
            WAIT_EVENT_CLIENT_READ
        );

        if (rc & WL_LATCH_SET) {
            ResetLatch(MyLatch);
            CHECK_FOR_INTERRUPTS();
        }

        if (rc & WL_EXIT_ON_PM_DEATH || ProcDiePending)
        {
            return; /* Postmaster died, exit gracefully */
        } 
		else if(!(rc & WL_SOCKET_READABLE))
        {
            /* Should not happen, but be safe */
            continue;
        }

        n = recvmsg(MyControlFd, &msg, 0);

        if (n != sizeof(cs->raddr))
        {
            int save_errno = errno;

            /* Check for orderly shutdown */
            if (n == 0 || save_errno == ECONNRESET || save_errno == ENOTCONN)
            {
                elog(DEBUG2, "control channel closed by postmaster");
                return;
            }

            /* Log error but continue waiting */
            elog(WARNING, "recvmsg failed on control channel: %m");
            continue;
        }

        /* Extract the file descriptor */
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg))
        {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS)
            {
                memcpy(&cs->sock, CMSG_DATA(cmsg), sizeof(int));
                break;
            }
        }

        if (cs->sock == PGINVALID_SOCKET)
        {
            elog(WARNING, "received message without file descriptor");
            continue;
        }
		else if (cs->sock < 0)
        {
            elog(WARNING, "received invalid file descriptor: %d", (int) cs->sock);
            closesocket(cs->sock);
            continue;
        }
        elog(DEBUG2, "received client socket (fd=%d) from postmaster", (int) cs->sock);
		return;
    }
}

static void
cleanup_pool_backend_socket(int code, Datum arg)
{
    char sock_path[MAXPGPATH];
  	if (is_reuse_cleanup)
		return;

    if (MyControlFd >= 0)
    {
        closesocket(MyControlFd);
        MyControlFd = -1;
    }
    
    snprintf(sock_path, sizeof(sock_path),
                "%s/%s.%d", connection_pool_socket_dir, connection_pool_socket_file_prefix, (int)MyProcPid);
    unlink(sock_path);
}

void
init_pool_backend_socket(void)
{
    char sock_path[MAXPGPATH];
    struct sockaddr_un addr;
    mode_t old_umask;

    /* 生成固定路径 */
    snprintf(sock_path, sizeof(sock_path),
             "%s/%s.%d", connection_pool_socket_dir, connection_pool_socket_file_prefix, (int)MyProcPid);

    /* 创建 Unix socket */
    MyListenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (MyListenFd < 0)
        elog(ERROR, "could not create control socket: %m");

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, sock_path, sizeof(addr.sun_path));

    /* 删除可能残留的 socket 文件 */
    unlink(sock_path);

    /* 设置安全权限 (0600) */
    old_umask = umask(0077);
    if (bind(MyListenFd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
        closesocket(MyListenFd);
        elog(ERROR, "could not bind control socket to '%s': %m", sock_path);
    }
    umask(old_umask);

    listen(MyListenFd, 1);

	on_proc_exit(cleanup_pool_backend_socket, 0);
}
