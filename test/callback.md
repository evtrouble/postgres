2.1 BackendInitialize / 通讯层

1. on_proc_exit(socket_close, 0)
   - 注册位置： pq_init() pqcomm.c
   - 作用：
     - 在进程退出时关闭客户端 socket，释放 FeBeWaitSet 等与连接相关的资源。
   - 进程复用下是否需要：
     - 对「单个连接结束」而言， 必须要有 等价的关闭逻辑，否则旧连接的 socket/事件集不会被正确回收。
     - 你目前的方案是通过 ReuseProcExitCleanup() 去执行所有 on_proc_exit ，所以会调用到 socket_close ，这对“结束当前连接”是符合预期的。
     - 结论： 在连接结束（复用前清理阶段）应该执行 ；真正进程退出时也要执行一次没问题。
2.2 InitProcess 相关

2. on_shmem_exit(MarkPostmasterChildInactive, 0)
   
   - 注册位置： RegisterPostmasterChildActive() pmsignal.c
   - 作用：
     - 在共享内存的 PMSignalState->PMChildFlags 中，把当前子进程 slot 从 PM_CHILD_ACTIVE/PM_CHILD_WALSENDER 标记回 PM_CHILD_ASSIGNED ，告诉 postmaster：这个子进程不再使用共享内存。
   - 进程复用下是否需要：
     - 对“真正的进程终止”必须执行，否则 postmaster 会认为这个 PGPROC 仍然 active。
     - 但对“仅结束一个连接、进程留在连接池中等待复用”：
       - 进程仍然在使用共享内存（PGPROC、缓冲区、锁子系统等都还在），如果在复用清理阶段就调用 MarkPostmasterChildInactive ，postmaster 认为该 slot 不再使用 shared memory，但实际还在用，语义是错误的。
     - 结论：
       - 复用清理阶段应该跳过
       - 只在真正的进程终止时调用 。
3. on_shmem_exit(ProcKill, 0)
   
   - 注册位置： InitProcess() proc.c
   - 作用：
     - 释放 LWLock（ LWLockReleaseAll() ）、取消 condition variable 等待
     - 把 MyProc 从当前 backend 解绑：重置 MyProc/ MyProcNumber ，并把 PGPROC 放回相应 freelist 中（ dlist_push_tail(procgloballist, &proc->links) ）
     - 更新 ProcGlobal->spins_per_delay ，唤醒 autovac launcher 等
   - 进程复用下是否需要：
     - 对“真正的进程终止”是必须要执行的。
     - 对“连接复用”来说，如果在 ReuseProcExitCleanup() 里执行 ProcKill ：
       - MyProc 会被清空， PGPROC 归还 freelist
       - 之后再进入下一次 MultiPostgresMain() / InitPostgres() 时，你并没有重新调用 InitProcess() ，整个进程就变成一个“没有 PGPROC 的普通进程”，大量代码假定 MyProc != NULL ，会出问题。
     - 结论：
       - 复用清理阶段不能执行 ProcKill ，否则 PGPROC 会被释放，彻底失去“进程复用”的基础。
       - 应当像你对 RemoveProcFromArray 那样，用 is_reuse_cleanup 在 shmem_exit() 里 special-case 掉它。
4. on_shmem_exit(RemoveProcFromArray, 0)
   
   - 注册位置： InitProcessPhase2() proc.c
   - 作用：
     - 从 ProcArray 中移除当前 PGPROC ，对事务可见性、快照计算等来说，这是“该后台进程不再存在”的标志。
   - 你的修改：
     - 在 shmem_exit() 中增加了：
       ```
       if (is_reuse_cleanup &&
           on_shmem_exit_list
           [on_shmem_exit_index].
           function == 
           RemoveProcFromArray)
           continue;
       ``` ipc.c
     - InitProcessPhase2() 里也对 ProcArrayAdd(MyProc) 做了 if (!is_reuse_cleanup) 保护。
   - 进程复用下是否需要：
     - 连接复用场景里：PGPROC 在整个进程生命周期内被复用，多次服务不同连接，因此从 ProcArray 中移除/重新加入是没必要甚至有害的。
     - 结论：
       - 对“连接复用的清理阶段”： 不应该调用 （你已经通过 is_reuse_cleanup 跳过，这是正确方向）。
       - 对“真正进程退出”：必须调用一次，把 PGPROC 从 ProcArray 中彻底移除。
2.3 BaseInit 里注册的回调

5. before_shmem_exit(pgstat_shutdown_hook, 0)
   
   - 注册位置： pgstat_initialize() pgstat.c
   - 作用：
     - 把当前 backend 的统计信息刷入共享统计系统 / 持久化
     - 删除本 backend 的统计 entry ( pgstat_drop_entry(...) )
     - 调 pgstat_detach_shmem() ，从 stats 共享内存 detach
   - 进程复用下是否需要：
     - 这是“一个 backend 进程彻底退出”时的逻辑；执行后当前 backend 在统计系统中完全不可见。
     - 你的 MultiPostgresMain 使用 static bool call_once 只在第一次进入时调用 BaseInit() ，后续复用连接不会再走 pgstat_initialize() 。 一旦 ReuseProcExitCleanup() 执行了 pgstat_shutdown_hook ，后续连接因为没有重新初始化 pgstat，就失去统计能力。
     - 结论：
       - 复用清理阶段不应该执行 pgstat_shutdown_hook 。
       - 它应该只在真正进程退出时执行一次。
6. before_shmem_exit(pgaio_shutdown, 0)
   
   - 注册位置： pgaio_init_backend() aio_init.c
   - 作用：
     - 关闭 AIO backend 状态、释放资源。
   - 进程复用下是否需要：
     - 和 pgstat 同样， pgaio_init_backend() 也是在 BaseInit() 中只调用一次；如果在复用清理阶段执行了 pgaio_shutdown ，后续连接不会重新调用 pgaio_init_backend ，AIO 就变成禁用状态。
     - 结论：
       - 复用清理阶段不应该执行 ，只在真正进程退出时执行一次较合理。
7. on_proc_exit(smgrshutdown, 0)
   
   - 注册位置： smgrinit() smgr.c
   - 作用：
     - 遍历所有 smgr 实现的 smgr_shutdown 回调，关闭 smgr 相关内部状态。
   - 进程复用下是否需要：
     - smgr 层有明确的事务级与进程级资源管理（ AtEOXact_SMgr 等），正常连接结束时应当已经把“事务级资源”释放。 smgrshutdown 更偏向于“进程彻底退出时的全局清理”。
     - 如果在每次复用清理阶段都执行 smgrshutdown ，但不重新调用 smgrinit() ，可能导致后续连接使用 smgr 时状态异常。
     - 结论：
       - 倾向于： 不在复用清理阶段执行 ，只在进程最终退出时执行一次。
8. on_shmem_exit(AtProcExit_Buffers, 0)
   
   - 注册位置： InitBufferManagerAccess() bufmgr.c
   - 作用：
     - UnlockBuffers(); 释放所有缓冲区锁
     - CheckForBufferLeaks(); 断言没有 buffer pin 泄漏
     - AtProcExit_LocalBuffers(); 本地 buffer 清理
   - 进程复用下是否需要：
     - 对每个连接结束时，确保没有 buffer pin / buffer 锁泄漏是 非常有必要 的。
     - 这一类“检查并清空 backend 自身状态”的回调，可以安全地在“复用清理阶段”执行。
     - 结论：
       - 复用清理阶段应该执行 ，有助于确保下一个连接在干净环境上运行。
9. before_shmem_exit(BeforeShmemExit_Files, 0)
   
   - 注册位置： InitTemporaryFileAccess() fd.c
   - 作用：
     - 调用 CleanupTempFiles(false, true) ，清理所有 temp file，并在 assert 模式下禁止再创建 temp file： fd.c
   - 进程复用下是否需要：
     - 清理所有临时文件对复用是有利的；但是它同时在 USE_ASSERT_CHECKING 下把 temporary_files_allowed=false ，意味着同一进程后续再创建 temp file 就会触发断言。
     - 考虑到 InitTemporaryFileAccess() 在 BaseInit 里只调一次，复用场景下不会重新允许 temp files。
     - 结论：
       - 从功能上讲“清理 temp file”是希望执行的，但这个 hook 还包含“禁止后续创建 temp file”的语义。
       - 进程复用下更合理做法，是在复用清理阶段手动调用 CleanupTempFiles(false, true) 一类函数，而 不要执行整个 BeforeShmemExit_Files 回调 。
10. before_shmem_exit(ReplicationSlotShmemExit, 0)
    
    - 注册位置： ReplicationSlotInitialize() slot.c
    - 作用：
      - 如果有 MyReplicationSlot ，释放之
      - 清理临时 replication slots
    - 进程复用下是否需要：
      - 每个连接结束时释放 replication slot 是正确的（不然 slot 会被占用）。
      - 该初始化是 per-process 的，但这个回调本质是“把所有 slot 状态恢复到干净”，非常适合在“每个连接结束时”执行。
      - 结论：
        - 复用清理阶段应该执行 ，不会破坏后续连接，只是确保 slot 不被泄露。
11. （可选） on_shmem_exit(print_lwlock_stats, 0)
    
    - 注册位置： lwlock_stats_init() （在 LWLOCK_STATS 宏下） lwlock.c
    - 作用：
      - 调试用，退出时打印 LWLock 统计到 stderr。
    - 进程复用下是否需要：
      - 只在 debug / profiling 环境才会启用，本质无状态副作用。
      - 即使每次“复用清理”执行，也只是多打一些调试日志。
      - 结论： 无强约束 ，可以保留。
2.4 InitPostgres 内部回调

12. on_shmem_exit(pgstat_beshutdown_hook, 0)
    
    - 注册位置： pgstat_beinit() backend_status.c
    - 作用：
      - 清理本 backend 在 PgBackendStatus 数组中的 entry 等。
    - 进程复用下是否需要：
      - 这是“该 backend 不再作为一个会话存在”的标志，在连接复用模型里，每个连接结束时可以视为“旧会话结束，一个新的会话稍后在同一进程上启动”，因此每次连接结束执行一次是合理的，只要新的会话重新调用 pgstat_bestart_initial/ final 。
      - 你的 MultiPostgresMain 中每次调用 InitPostgres() ，其中 pgstat_bestart_initial() 会重新初始化状态，因此多次执行 pgstat_beshutdown_hook 是可以接受的。
      - 结论：
        - 复用清理阶段可以执行 。
13. on_shmem_exit(CleanupInvalidationState, PointerGetDatum(segP))
    
    - 注册位置： SharedInvalBackendInit(false) sinvaladt.c
    - 作用：
      - 把当前 backend 在 sinval 队列中的 state 标记为不活跃，并维护 nextLXID 等。
    - 进程复用下是否需要：
      - 每个“会话生命周期”结束时清理 sinval 状态是合理的；新的会话再次调用 SharedInvalBackendInit(false) 重建状态即可。
      - 结论：
        - 复用清理阶段可以执行 。
14. on_shmem_exit(CleanupProcSignalState, 0)
    
    - 注册位置： ProcSignalInit() procsignal.c
    - 作用：
      - 把当前 backend 的 procsignal 条目标记为不再使用。
    - 进程复用下是否需要：
      - 和 sinval 类似，每个会话结束时清理状态是合理的，只要新的会话重新调用 ProcSignalInit() 。
      - 结论：
        - 复用清理阶段可以执行 。
15. before_shmem_exit(ShutdownPostgres, 0)
    
    - 注册位置： InitPostgres() postinit.c
    - 作用：
      - 这是整套“数据库层”关闭逻辑的总入口，负责：
        - 中止/提交剩余事务
        - relcache/syscache/portal/plancache 等清理
        - 事务级资源回收
    - 进程复用下是否需要：
      - 对每个连接结束时，确实希望把事务、relcache 等 session 级别状态清干净，否则下一个连接会继承一堆乱七八糟的 session 状态。
      - 你已经在 ResetBackendForReuse() 里做了一些手工清理（ ResetAllOptions() 、重置各种 timeout/标志、Reset memory context 等），但完全复用原本的 ShutdownPostgres 能覆盖更多 corner case。
      - 结论：
        - 复用清理阶段应该执行 ，它属于“会话级别的清理”，再复用前执行一次是比较安全的。
2.5 PostgresMain 尾部

16. on_proc_exit(log_disconnections, 0)
    - 注册位置： PostgresMain() postgres.c
    - 作用：
      - 在进程退出时记录断开连接的日志（ LOG: disconnection: ... ）。
    - 进程复用下是否需要：
      - 这是“backend 进程终止时”的日志，而不是“连接结束时”的日志。
      - 在连接复用模型中，如果你打算保留“每个连接断开都打 disconnection log”，就需要把这类逻辑改造成“每个连接结束时显式调用”而不是依赖 proc_exit 。
      - 目前通过 ReuseProcExitCleanup() 去调用所有 on_proc_exit ，会在每个连接结束时触发 log_disconnections ，这个行为其实是符合你的需求的（只是名字依旧是 proc_exit hook）。
      - 结论：
        - 行为上来看： 复用清理阶段执行是可以接受的 （会多一条 disconnection log），真正进程退出时再写一条也没问题。
3. 结合进程复用的建议总结

从上述分析可以把回调分三类来看：

- A. 连接结束时必须执行（会话级清理）
  
  - socket_close （关闭旧连接的 socket）
  - AtProcExit_Buffers （释放 buffer 锁和 pin，检查泄漏）
  - ReplicationSlotShmemExit （释放 replication slots）
  - pgstat_beshutdown_hook （结束该会话的 backend status）
  - CleanupInvalidationState
  - CleanupProcSignalState
  - ShutdownPostgres （会话级的数据库清理，事务/relcache/syscache 等）
- B. 只能在真正进程退出时执行一次（进程级 teardown）
  
  - ProcKill （释放 PGPROC，自此进程不再作为 backend）
  - RemoveProcFromArray （从 ProcArray 移除 PGPROC）——你已经在复用清理阶段跳过
  - MarkPostmasterChildInactive （告诉 postmaster 该 child 不再使用 shared memory）
  - pgstat_shutdown_hook （ detach 统计共享内存、删除 backend stats entry）
  - pgaio_shutdown
  - smgrshutdown
  - BeforeShmemExit_Files （其中“禁止再创建 temp file”的语义明显是进程级的）
- C. 调试或影响较小的回调
  
  - print_lwlock_stats （ LWLOCK_STATS ）
  - 部分模块的 debug-only shutdown hook（这里未一一列出）
对你现在的实现（ ReuseProcExitCleanup + is_reuse_cleanup ）而言：

- 你已经对 RemoveProcFromArray 做了正确特殊处理，这是“B 类”的代表；
- 但 ProcKill / MarkPostmasterChildInactive / pgstat_shutdown_hook / pgaio_shutdown / BeforeShmemExit_Files / smgrshutdown 目前在复用清理时仍然会被 shmem_exit / on_proc_exit 调用，这会破坏「这个进程被设计成长期活着并继续作为 backend 使用」这一前提：
  - ProcKill ：直接把 MyProc 回收，后续一切基于 PGPROC 的逻辑都会乱；
  - MarkPostmasterChildInactive ：postmaster 认为该子进程不再使用 shared memory；
  - pgstat_shutdown_hook / pgaio_shutdown / BeforeShmemExit_Files ：把只在 BaseInit 中做一次的全局子系统关掉，却不会在下一连接重新初始化；
  - smgrshutdown ：类似全局子系统关闭问题。
因此，在“进程复用”模型下，如果你继续沿用“模拟 proc_exit 的方式做复用清理”，建议你：

- 在 shmem_exit() / ReuseProcExitCleanup() 里，把 B 类回调统一做 is_reuse_cleanup 保护（类似现在跳过 RemoveProcFromArray 的方式）；
- A 类会话级清理保留，让每次连接结束都“干净退场”。