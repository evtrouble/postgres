# 连接分配机制实现说明

## 问题描述

当前实现中，从连接池获取空闲后端后，连接分配机制尚未实现。需要将新连接的 socket 传递给已存在的后端进程，并让后端进程接收并处理新连接。

## 当前状态

### Postmaster 端（ServerLoop）

```c
if (enable_connection_pool)
{
    PMChild *idle_backend = PoolGetIdleBackend();
    
    if (idle_backend != NULL)
    {
        // TODO: 实现连接分配
        // 当前：只是打印日志，然后创建新后端
        elog(DEBUG2, "got idle backend from pool (pid=%d), but connection assignment not implemented yet, creating new backend",
             (int) idle_backend->pid);
        // Fall through to create new backend
    }
}

// 创建新后端（即使有空闲后端也会执行）
BackendStartup(&s);
```

### Backend 端（PostgresMain）

```c
// 后端进程在完成连接后入队并等待
if (PoolEnqueuePMChild(MyPMChildPtr))
{
    for (;;)
    {
        WaitLatch(MyLatch, ...);
        ResetLatch(MyLatch);
        
        // TODO: 检查是否有新连接分配
        // 当前：只检查 SIGTERM，然后继续等待
    }
}
```

## 需要实现的功能

### 1. Socket 传递机制

需要将新连接的 socket 从 postmaster 传递给已存在的后端进程。有几种实现方式：

#### 方案 A：Unix Domain Socket（推荐）

- **原理**：使用 Unix domain socket 的 `SCM_RIGHTS` 机制传递文件描述符
- **实现**：
  1. 每个后端进程在启动时创建一个 Unix domain socket（或使用共享的 socket pair）
  2. Postmaster 通过 `sendmsg()` 将新连接的 socket 发送给后端进程
  3. 后端进程通过 `recvmsg()` 接收 socket
- **优点**：
  - 标准 Unix 机制，跨平台支持好
  - 性能好，无需额外开销
- **缺点**：
  - 需要管理 socket 的生命周期
  - Windows 平台需要特殊处理

#### 方案 B：共享内存 + 信号

- **原理**：在共享内存中存储 socket 信息，通过信号唤醒后端
- **实现**：
  1. 在共享内存中为每个后端进程预留一个 slot，存储 socket 信息
  2. Postmaster 将 socket 信息写入共享内存
  3. Postmaster 通过 `SetLatch()` 或信号唤醒后端进程
  4. 后端进程从共享内存读取 socket 信息
- **优点**：
  - 实现相对简单
  - 不需要额外的 socket
- **缺点**：
  - Socket 不能直接存储在共享内存中，需要其他方式传递
  - 需要额外的同步机制

#### 方案 C：重新初始化连接循环

- **原理**：让后端进程重新进入 `BackendInitialize` 和 `PostgresMain` 循环
- **实现**：
  1. 后端进程在等待循环中，当被唤醒时，检查是否有新连接
  2. 如果有新连接，重新调用 `BackendInitialize()` 和 `PostgresMain()`
  3. 需要保存和恢复必要的状态
- **优点**：
  - 可以重用现有的初始化代码
- **缺点**：
  - 需要处理状态保存和恢复
  - 可能比较复杂

### 2. 后端进程唤醒机制

后端进程在等待循环中需要能够：
1. 被 postmaster 唤醒（当有新连接时）
2. 接收新连接的 socket
3. 重新初始化连接并开始处理

### 3. 状态管理

后端进程在等待重用期间需要：
1. 清理前一个连接的状态
2. 保持必要的进程状态（如共享内存连接）
3. 准备接收新连接

## 推荐实现方案

### 使用 Unix Domain Socket + Latch

1. **初始化阶段**：
   - 每个后端进程在启动时创建一个 Unix domain socket
   - 将 socket 路径存储在共享内存或通过其他方式传递给 postmaster

2. **连接分配阶段**（Postmaster）：
   ```c
   if (idle_backend != NULL)
   {
       // 通过 Unix domain socket 发送新连接的 socket
       if (send_socket_to_backend(idle_backend, client_sock))
       {
           // 唤醒后端进程
           SetLatch(idle_backend->procLatch);
           // 不创建新后端
           return;
       }
   }
   // 如果分配失败，创建新后端
   BackendStartup(&s);
   ```

3. **等待循环阶段**（Backend）：
   
   **⚠️ 注意：避免栈调用加深**
   
   如果在等待循环中递归调用 `PostgresMain()`，会导致栈不断增长：
   ```
   PostgresMain() 
     -> 处理连接
     -> 等待循环
       -> BackendInitialize()
       -> PostgresMain()  // 递归调用！
         -> 处理连接
         -> 等待循环
           -> BackendInitialize()
           -> PostgresMain()  // 再次递归
             -> ...
   ```
   
   **推荐方案：在 PostgresMain 最外层添加连接循环**
   
   修改 `PostgresMain()` 结构，在最外层添加连接处理循环：
   ```c
   void PostgresMain(const char *dbname, const char *username)
   {
       // 初始化代码（只执行一次）
       // ...
       
       // 连接处理循环（处理多个连接）
       for (;;)
       {
           // 如果是第一次调用或连接池重用，初始化连接
           if (need_connection_init)
           {
               BackendInitialize(new_sock, CAC_OK);
               InitProcess();  // 可能需要重新初始化
               // 其他必要的初始化
           }
           
           // 原有的 PostgresMain 主循环（处理单个连接的命令）
           for (;;)
           {
               // 处理命令...
               if (Terminate || EOF)
               {
                   // 清理连接状态
                   // ...
                   
                   if (enable_connection_pool && MyPMChildPtr != 0)
                   {
                       // 入队并等待新连接
                       PoolEnqueuePMChild(MyPMChildPtr);
                       
                       // 等待新连接分配
                       for (;;)
                       {
                           WaitLatch(MyLatch, ...);
                           ResetLatch(MyLatch);
                           
                           // 检查是否有新连接
                           ClientSocket *new_sock = receive_socket_from_postmaster();
                           if (new_sock != NULL)
                           {
                               // 设置标志，继续外层循环
                               need_connection_init = true;
                               new_client_sock = new_sock;
                               break;  // 退出等待循环，继续外层循环
                           }
                           
                           if (ProcDiePending)
                               proc_exit(0);
                       }
                       // 继续外层循环，重新初始化连接
                       continue;
                   }
                   else
                   {
                       // 未启用连接池，退出
                       proc_exit(0);
                   }
               }
           }
       }
   }
   ```
   
   这样避免了递归调用，栈深度保持恒定。

## 实现步骤

1. **添加 socket 传递函数**：
   - `send_socket_to_backend()`: Postmaster 发送 socket 给后端
   - `receive_socket_from_postmaster()`: 后端接收 socket

2. **修改 BackendStartup**：
   - 在创建后端时，建立 socket 传递通道
   - 或者使用共享的 socket pair

3. **修改 PostgresMain 等待循环**：
   - 在等待循环中检查是否有新连接
   - 如果有，重新初始化并处理

4. **状态管理**：
   - 确保在重新初始化前清理前一个连接的状态
   - 保持必要的进程状态

## 注意事项

1. **Socket 传递**：需要处理 socket 传递失败的情况，回退到创建新后端
2. **错误处理**：如果后端进程在等待期间出现问题，需要能够检测并处理
3. **并发安全**：确保 socket 传递的原子性
4. **平台兼容**：Windows 平台可能需要不同的实现方式
5. **性能考虑**：Socket 传递应该尽可能高效，避免成为瓶颈

## 相关代码位置

- **Postmaster 端**：`src/backend/postmaster/postmaster.c:1736-1754`
- **Backend 端**：`src/backend/tcop/postgres.c:5060-5084`
- **Socket 传递**：需要新增函数，可能放在 `src/backend/postmaster/connection_pool.c` 或新文件

