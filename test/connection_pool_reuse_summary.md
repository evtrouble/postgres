# 内置连接池复用问题与修复总结

## 症状与背景
  - 在后端进程复用（连接池）模型下，结束一个会话并复用同一进程为下一会话时，日志出现多条 WARNING: leaking still-referenced relcache entry for "pg_xxx"（例如 pg_class、pg_proc、pg_type 及其索引）。
  - 另有现象：复用后的会话未重新向客户端报告 server_version 等带 GUC_REPORT 的参数。

## 根因与分析
  - relcache 泄漏 WARNING 来源于 [relcache.c:RelationCacheInsert 宏](file:///home/gyl/c/postgres/src/backend/utils/cache/relcache.c#L209-L229)：当为同一 OID 以“新 Relation”覆盖旧条目且旧条目 refcount > 0 时会发出泄漏告警。
  - 会话结束的清理在 PG 原生模型中通过 before_shmem_exit/on_shmem_exit 以及 ShutdownPostgres、AtEOXact_* 等回调链完成；进程彻底退出再做一次进程级清理。
  - 连接复用时，如果错误地执行“进程级 teardown”的回调（例如 ProcKill、MarkPostmasterChildInactive、pgstat_shutdown_hook、pgaio_shutdown、smgrshutdown、BeforeShmemExit_Files 等），会破坏后续会话所需的长生命周期状态，造成异常。

## 修复方案与改动
  - 明确会话级 vs 进程级清理边界
    - 通过 is_reuse_cleanup 标识“复用清理”阶段，仅执行会话级清理回调，跳过进程级 teardown：
      - 已在多个模块中对进程级回调加保护，例如：
        - [proc.c:ProcKill](file:///home/gyl/c/postgres/src/backend/storage/lmgr/proc.c#L920-L933)（加 is_reuse_cleanup 直接返回）
        - [pmsignal.c:MarkPostmasterChildInactive](file:///home/gyl/c/postgres/src/backend/storage/ipc/pmsignal.c#L325-L337)
        - [pgstat.c:pgstat_shutdown_hook](file:///home/gyl/c/postgres/src/backend/utils/activity/pgstat.c#L604-L635)
        - [fd.c:BeforeShmemExit_Files](file:///home/gyl/c/postgres/src/backend/storage/file/fd.c#L3240-L3251)
        - [smgr.c:smgrshutdown](file:///home/gyl/c/postgres/src/backend/storage/smgr/smgr.c#L210-L226)
      - 在 [ipc.c:ReuseProcExitCleanup](file:///home/gyl/c/postgres/src/backend/storage/ipc/ipc.c#L161-L192) 中统一设置 is_reuse_cleanup，并执行 before_shmem_exit/on_shmem_exit 回调，但对 RemoveProcFromArray 等进行 special-case 跳过。
  - 修复 GUC 报告状态
    - 增加 ResetGUCReporting，使复用后的会话重新报告带 GUC_REPORT 的参数（如 server_version）。
    - 在 [backend_startup.c:ResetBackendForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L88-L142) 中调用 ResetAllOptions + ResetGUCReporting。
  - 复用时“重建哈希/缓存”的安全路径
    - 不重复调用 RelationCacheInitialize/InitCatalogCache/RelationCacheInitializePhase3（它们属于进程启动期逻辑，仅首次 InitPostgres 需要）。
    - 在每次复用清理结束后，统一走系统提供的“缓存失效入口”：
      - InvalidateSystemCachesExtended(false)  
        作用：清空/失效 catcache、relcache（含 smgr/relmap），并触发各类 cache 的回调（typcache、plancache 等），下次访问再懒加载。
      - 已在 [backend_startup.c:ResetBackendForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L88-L142) 加入，并增加头文件 [utils/inval.h](file:///home/gyl/c/postgres/src/include/utils/inval.h) 引用。
    - 对 RelationCacheInitializePhase3 的结论：仅在首次会话（InitPostgres）运行一次，后续复用会话跳过是可行且推荐；该阶段会加载关键系统索引并将“伪”条目修正为真实 pg_class 等数据，属进程级 bootstrap。

## 关键改动与代码参考
  - 复用清理流程：
    - [backend_startup.c:ResetBackendForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L88-L142)  
      - 保持 ReuseProcExitCleanup（运行会话级 before_shmem_exit/on_shmem_exit，包括 ShutdownPostgres）  
      - 重置 GUC/超时/Latch/错误上下文等轻量状态  
      - 调用 InvalidateSystemCachesExtended(false) 丢弃系统缓存（统一、安全）
  - 会话级/进程级回调保护：
    - [ipc.c:ReuseProcExitCleanup](file:///home/gyl/c/postgres/src/backend/storage/ipc/ipc.c#L161-L192)
    - [smgr.c:smgrshutdown](file:///home/gyl/c/postgres/src/backend/storage/smgr/smgr.c#L210-L226)
    - [pgstat.c:pgstat_shutdown_hook](file:///home/gyl/c/postgres/src/backend/utils/activity/pgstat.c#L604-L635)
    - [pmsignal.c:MarkPostmasterChildInactive](file:///home/gyl/c/postgres/src/backend/storage/ipc/pmsignal.c#L325-L337)
    - [fd.c:BeforeShmemExit_Files](file:///home/gyl/c/postgres/src/backend/storage/file/fd.c#L3240-L3251)
  - relcache 初始化与 Phase3：
    - [relcache.c:RelationCacheInitializePhase3](file:///home/gyl/c/postgres/src/backend/utils/cache/relcache.c#L4106-L4383)
    - [postinit.c:InitPostgres 阶段调用位置](file:///home/gyl/c/postgres/src/backend/utils/init/postinit.c#L1672-L1682)
  - 统一缓存失效入口：
    - [inval.c:InvalidateSystemCachesExtended](file:///home/gyl/c/postgres/src/backend/utils/cache/inval.c#L785-L813)

## 复用流程建议
  1. 首次会话：走 InitPostgres → RelationCacheInitialize/InitCatalogCache/EnablePortalManager → RelationCacheInitializePhase2/Phase3 → 注册 ShutdownPostgres 等回调。
  2. 会话结束：调用 ReuseProcExitCleanup（会话级清理，跳过进程级 teardown）。
  3. 复用前重置：ResetBackendForReuse → ResetAllOptions + ResetGUCReporting → InvalidateSystemCachesExtended(false)。
  4. 下一会话：走 ReusePostgres（不再调用 Phase3），按需懒加载 relcache/catcache/plan cache 等。

## 为什么当前只能同库复用

- 后端进程在 PG 的经典模型里天然绑定单个数据库：首次连接完成认证后，进程会进入某个数据库上下文并长期持有大量“数据库级”的全局状态（典型如 MyDatabaseId、数据库目录/表空间信息、PGPROC/ProcArray 中记录的 databaseId 等），这些状态会参与锁管理、资源所有者、系统缓存键空间与失效逻辑。
- 复用路径当前的定位是“轻量会话重置”：ResetBackendForReuse/ReuseProcExitCleanup 只保证会话级清理与统一缓存失效，避免触发进程级 teardown，但它并不等价于重新执行一次 InitPostgres（尤其是 Phase3/关键系统索引加载、数据库级目录切换、统计/快照/权限相关状态的重新建立）。
- 若允许跨库复用，需要证明并实现：所有与旧数据库绑定的状态都能被完整拆除并在新数据库上重建，且不会把旧库的锁、缓存条目、临时对象、prepared 计划、统计/权限/命名空间等残留带到新库；这在当前实现中既未覆盖也难以验证正确性。
- 因此当前策略是只做同库复用：postmaster 在分配新连接时解析 startup packet 的 database（MSG_PEEK，不消费数据），只从连接池里挑选 last_dbname/last_db_hash 匹配的空闲后端复用；不匹配则新建后端或淘汰空闲后端。

- 后续验证建议
  - 在连接池高复用压测场景下观察：
    - 是否仍出现 relcache 泄漏 WARNING（若有，记录具体对象与调用栈，重点关注是否存在自定义路径在持有 refcount 时替换 relcache 条目）。
    - 复用后，客户端是否收到 server_version 等 GUC_REPORT 参数（确认 ResetGUCReporting 生效）。
    - 若需要跨数据库复用，同一后端服务不同数据库，建议改为每次切库重新走 InitPostgres（包含 Phase3），而非轻量的 ReusePostgres。

## 关停与信号处理

- 问题现象
  - 执行 pg_ctl stop 时，进程池中的空闲后端不退出，postmaster 长时间等待（PM_WAIT_BACKENDS）；日志中出现持续的队列 peek 行为。
  - 个别环境下出现 [postgres] <defunct> 僵尸进程。

- 根因分析
  - 后端在控制信道上阻塞于 accept()/recvmsg，受 SA_RESTART 影响对 SIGTERM 响应延迟；
  - 复用后的第二次会话初始化阶段，将 SIGTERM 暂时设置为 process_startup_packet_die 并应用 StartupBlockSig/BlockSig；会话完成后未恢复为 die/UnBlockSig，导致 SIGTERM 到达时不能设置 ProcDiePending/唤醒 Latch，从而卡在 WaitLatchOrSocket。
  - 复用后端的中断屏蔽计数未复位，进一步导致信号处理被延迟。

- 修复方案
  - 后端控制信道改为基于 Latch 的可中断等待
    - 将 accept/recvmsg 包裹到 WaitLatchOrSocket，使用 WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH，并在唤醒后执行 CHECK_FOR_INTERRUPTS，确保 SIGTERM 能被及时处理。
    - 代码位置： [pm_backend_comm.c:receive_socket_from_postmaster](file:///home/gyl/c/postgres/src/backend/postmaster/pm_backend_comm.c#L134-L173) 与后续 recvmsg 等待路径 [pm_backend_comm.c:207-271](file:///home/gyl/c/postgres/src/backend/postmaster/pm_backend_comm.c#L207-L271)。
  - 复用初始化结束恢复 SIGTERM→die 并解除屏蔽
    - 在 BackendInitializeForReuse 末尾恢复 pqsignal(SIGTERM, die) 并执行 sigprocmask(SIG_SETMASK, &UnBlockSig, NULL)，确保复用后端在空闲等待期间可被 SIGTERM 正常打断并退出。
    - 代码位置：函数 [BackendInitializeForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L259-L451) 尾部。
  - 复用重置补强
    - 在复用重置中增加 InterruptHoldoffCount 与 CritSectionCount 归零，避免信号处理被延迟。
    - 代码位置： [backend_startup.c:ResetBackendForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L88-L142)。
  - 认证阶段的安全信号处理
    - 在 die()/ProcessInterrupts 处理 SIGTERM 时，若处于 ClientAuthInProgress，避免向远端输出，必要时以 FATAL: canceling authentication due to timeout 收尾，保证尽快退出。
    - 代码位置： [postgres.c:ProcessInterrupts](file:///home/gyl/c/postgres/src/backend/tcop/postgres.c#L1-L400) 附近。
  - 连接分配路径的健壮性
    - 通过 Unix 域套接字向复用后端传递客户端 fd；若发送失败（如 Broken pipe），回退到创建新后端。
    - 代码位置：发送端 [pm_backend_comm.c:send_socket_to_backend](file:///home/gyl/c/postgres/src/backend/postmaster/pm_backend_comm.c#L26-L100)，接收端同上。

- 验证要点
  - 执行 pg_ctl stop -m fast -w 时，池中后端在空闲等待时收到 SIGTERM 能打印 die 相关日志并快速退出；postmaster 不再长时间停留在 PM_WAIT_BACKENDS。
  - 后端空闲等待点应为 WaitLatchOrSocket（epoll_wait），而不是裸 accept。
  - 不应残留 [postgres] <defunct>；如有，检查 postmaster 的 SIGCHLD 回收路径是否正常记录 “client backend (PID …) exited …”。

- 现象补充与注意
  - “为什么还在 peek”：仅当 Shutdown == NoShutdown 时仍使用 PoolCleanupExpired 的 peek；若关机期间仍见大量 peek 日志，需确认 Shutdown 状态更新是否达标，并检查后端是否已恢复 SIGTERM→die 与 UnBlockSig。
  - 控制信道断开：若发送端日志出现 “failed to send client socket … Broken pipe”，属预期保护分支，postmaster 将回退到新建后端。

- 相关代码参考
  - 控制信道等待与接收： [pm_backend_comm.c:receive_socket_from_postmaster](file:///home/gyl/c/postgres/src/backend/postmaster/pm_backend_comm.c#L134-L271)
  - fd 传递： [pm_backend_comm.c:send_socket_to_backend](file:///home/gyl/c/postgres/src/backend/postmaster/pm_backend_comm.c#L26-L100)
  - 复用重置： [backend_startup.c:ResetBackendForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L88-L142)
  - 复用初始化信号恢复： [backend_startup.c:BackendInitializeForReuse](file:///home/gyl/c/postgres/src/backend/tcop/backend_startup.c#L259-L451)
  - 中断处理： [postgres.c:ProcessInterrupts](file:///home/gyl/c/postgres/src/backend/tcop/postgres.c)



---

## 排查清单（新增）
- 锁释放告警
  - 记录 Unlock/LockRelease 调用点的锁标识（type/field1-4、mode），核对是否符合 fast-path 条件。
  - 检查是否存在跨库复用后残留的关系锁（field1 与 MyDatabaseId 不一致）。
  - 确认 ResetBackendForReuse 中是否执行了 LockReleaseAll(DEFAULT_LOCKMETHOD, true)。
- 关系缓存
  - 确认 RelationCacheInitializeForReuse 在 StartTransactionCommand 之后调用。
  - 检查 load_critical_index 是否命中已有条目并原位重载，而不是创建新条目。
  - 查看 criticalRelcachesBuilt/criticalSharedRelcachesBuilt 标志是否正确置位。
- 进程标题
  - idle/reset 路径是否只显示用户与远端，不显示数据库名。
  - 新连接到来时是否重建前缀并设置“initializing/active”等状态。

## 日志观察点（新增）
- 提交阶段崩溃堆栈若定位至 lock.c:2417，优先关注 fast-path 判定失败（EligibleForRelationFastPath 返回 false）的具体 locktag 与锁模式。
- RelationCacheInitializeForReuse 若出现 WARNING（CurrentResourceOwner is NULL），说明调用时序异常，应回溯事务启动点。
- 复用重置阶段建议临时以 DEBUG3 记录：
  - fast-path 关系锁计数（fpInfoLock 保护下的计数）。
  - 本地锁表条目数量。
 以便在高并发压测下快速定位残留来源。

## 操作建议（新增）
- 切库频繁的场景，优先确保在复用重置阶段做关系锁清理，再进行 MyDatabaseId 变更与后续复用初始化。
- 对于需要最小化日志噪音的生产环境：
  - 保留 RelationCacheInitializeForReuse 的断言，但将 WARNING 降为 LOG，仅在测试环境开启 WARNING。
  - 可在 GUC 下控制复用阶段的详细调试输出，便于按需开启。

---
