# 连接池并发模型说明

## 为什么当前设计不需要锁？

### 1. PostgreSQL 架构特点

**Postmaster 是单线程的：**
- PostgreSQL 的 postmaster 进程是**单线程**设计
- 所有操作都在 `ServerLoop()` 事件循环中**串行执行**
- 这是 PostgreSQL 的**核心设计原则**，确保稳定性和可预测性

**证据：**
```c
// postmaster.c 中的注释明确说明：
// "allows the auth code to be written in a simple single-threaded style"
// "More importantly, it ensures that blockages in non-multithreaded
//  libraries like SSL or PAM cannot cause denial of service"
```

### 2. 连接池操作的执行上下文

**所有连接池操作都在 ServerLoop 中：**
```c
ServerLoop() {
    for (;;) {
        // 1. 等待事件（连接、信号等）
        WaitEventSetWait(...);
        
        // 2. 处理新连接（串行处理）
        for (int i = 0; i < nevents; i++) {
            if (events[i].events & WL_SOCKET_ACCEPT) {
                // 这里调用连接池函数
                PoolGetIdleBackend();  // ← 单线程执行
            }
        }
        
        // 3. 处理其他任务（也是串行的）
        LaunchMissingBackgroundProcesses();
    }
}
```

**关键点：**
- 每次循环只处理一个连接
- 所有操作都是**原子性**的（在单线程中）
- 没有并发访问的可能性

### 3. 后端进程的独立性

**后端是独立进程，不是线程：**
```
Postmaster (单线程)
    ├─ Backend Process 1 (独立进程)
    ├─ Backend Process 2 (独立进程)
    └─ Backend Process 3 (独立进程)
```

**通信方式：**
- 通过 `fork()` 创建，不是 `pthread_create()`
- 通过**共享内存**和**信号**通信
- 后端进程**不会直接访问**连接池的数据结构
- 所有池操作都由 postmaster 统一管理

### 4. 信号处理的机制

**信号处理使用标志位模式：**
```c
// 信号处理函数（异步）
static volatile sig_atomic_t pending_pm_child_exit;

void handle_signal() {
    pending_pm_child_exit = true;  // 只设置标志
}

// 主循环中处理（同步）
ServerLoop() {
    if (pending_pm_child_exit) {
        process_pm_child_exit();  // 实际工作在这里做
    }
}
```

**关键点：**
- 信号处理函数**只设置标志**，不做实际工作
- 所有实际工作都在**主线程**中完成
- 避免了信号处理中的并发问题

## 什么时候可能需要锁？

### 场景 1：异步 I/O 操作
如果未来添加异步 I/O：
```c
// 假设的异步操作（需要锁）
void async_pool_operation() {
    // 可能在另一个线程/协程中执行
    // 需要 LWLock 保护
}
```

### 场景 2：后台维护线程
如果添加专门的维护线程：
```c
// 维护线程（需要锁）
void* maintenance_thread(void* arg) {
    while (1) {
        // 清理过期后端
        PoolCleanupExpired();  // ← 需要锁保护
    }
}
```

### 场景 3：后端直接访问池
如果允许后端进程直接操作池：
```c
// 后端进程中（需要锁）
void backend_notify_idle() {
    // 后端通知 postmaster 自己变为空闲
    // 需要进程间同步机制
}
```

## 如果未来需要锁，应该怎么做？

### 方案 1：使用 LWLock（推荐）
```c
#include "storage/lwlock.h"

static LWLock *pool_lock;

void InitConnectionPool(void) {
    pool_lock = GetNamedLWLockTranche("connection_pool", 1);
    // ...
}

void PoolAddBackend(PMChild *pmchild) {
    LWLockAcquire(pool_lock, LW_EXCLUSIVE);
    // ... 操作池
    LWLockRelease(pool_lock);
}
```

### 方案 2：使用自旋锁（高性能场景）
```c
#include "storage/spin.h"

static slock_t pool_spinlock;

void PoolGetIdleBackend(void) {
    SpinLockAcquire(&pool_spinlock);
    // ... 操作池
    SpinLockRelease(&pool_spinlock);
}
```

### 方案 3：使用原子操作（简单场景）
```c
// 对于简单的计数器
static pg_atomic_uint32 pool_size;

void increment_pool_size(void) {
    pg_atomic_add_fetch_u32(&pool_size, 1);
}
```

## 当前设计的优势

1. **简单高效**：无锁设计，性能最优
2. **易于理解**：单线程模型，逻辑清晰
3. **稳定可靠**：避免了锁竞争、死锁等问题
4. **符合 PostgreSQL 设计**：与现有架构一致

## 总结

**当前不需要锁的原因：**
1. ✅ Postmaster 是单线程的
2. ✅ 所有操作都在 ServerLoop 中串行执行
3. ✅ 后端是独立进程，不直接访问池
4. ✅ 信号处理只设置标志，实际工作在主线程

**未来可能需要锁的情况：**
- 添加异步操作
- 添加后台线程
- 允许后端直接访问池

**建议：**
- 保持当前无锁设计
- 如果未来需要并发，再添加 LWLock
- 在代码注释中明确说明并发模型

