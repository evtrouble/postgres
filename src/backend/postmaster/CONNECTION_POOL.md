# PostgreSQL 连接池实现文档

## 概述

PostgreSQL 内置连接池是一个无锁（lock-free）连接池实现，用于重用后端进程，减少进程创建和销毁的开销。连接池使用共享内存中的无锁队列来协调多个后端进程（生产者）和 postmaster 的 accept 循环（单消费者）。

## 架构设计

### 1. 无锁队列设计

- **队列存储**：队列存储 `PMChild*` 指针（作为 `uintptr_t` 地址值）
- **PMChild 结构**：在 postmaster 进程的私有内存中分配
- **指针传递**：后端进程在启动时接收 `PMChild*`，存储为 `uintptr_t`，仅存储不访问
- **入队操作**：多个后端进程在完成客户端连接后，原子性地将 `PMChild*` 入队
- **出队操作**：postmaster 的 accept 循环（单消费者）直接出队 `PMChild*`，无需查找

#### 1.1 误判降低是如何实现的

连接池里“误判”主要指两类：
- **把队列误判为空**：实际上已有后端完成入队，但 postmaster 在 `Peek/Dequeue` 时返回 `NULL`（通常是短暂的竞争窗口导致）。
- **把队列误判为满**：实际上存在可用槽位，但生产者 `Enqueue` 返回失败（同样通常是竞争窗口或计数/标识回绕导致）。

为降低这些误判，队列对每个槽位引入了一个单调递增的 `seq`（sequence）字段，并用它作为“槽位状态机”的唯一判据，而不是依赖 `pmchild_ptr != 0` 这种容易被写入重排/可见性影响的判断。

核心协议（bounded ring + per-slot sequence）：
- 初始化时：第 i 个槽位 `seq = i`，表示“该槽位可被写入，期望的位置是 i”
- 生产者入队（pos = tail）：
  - 只有当 `cell->seq == pos` 时，才表示该槽位“属于当前 pos，可写”
  - 成功占有 `tail` 后，先写入 `idle_since` 与 `pmchild_ptr`，再以发布语义把 `cell->seq` 写为 `pos + 1`，表示“该槽位已就绪，可被消费”
- 消费者出队/窥视（pos = head）：
  - 只有当 `cell->seq == pos + 1` 时，才表示该槽位“对当前 pos 已就绪，可读”
  - 出队完成后，以发布语义把 `cell->seq` 写为 `pos + capacity`，表示“该槽位已释放，下一轮可被写入”

之所以能降低误判，关键在于：
- **用 `seq` 把“占位/写数据/发布就绪”分离**：消费者只看 `seq` 是否到达“就绪态”，避免读到“指针还没写完/时间戳还没可见”的中间态。
- **在 `seq` 读写上使用内存屏障语义**：生产者用带 membarrier 的写把数据发布出去；消费者用带 membarrier 的读保证在观察到“就绪 seq”之后，后续读取到的 `pmchild_ptr/idle_since` 是同一次入队的数据，而不是旧值/部分可见的值。
- **`seq/head/tail` 使用 64 位避免回绕**：`pos` 会持续递增，`dif = seq - pos` 依赖“长时间单调”这一假设；若用较小位宽，回绕会把“空/满”判定翻转，导致长期的误判甚至卡死。

在当前连接池使用方式下，peek 返回 `NULL` 仍可能发生（生产者刚占到 `tail` 但尚未发布 `seq=pos+1`），但它只会是短暂的竞争窗口；随着一次循环或下一次事件触发，状态会稳定下来。

### 2. 并发模型

- **多生产者**：多个后端进程并发地将 `PMChild*` 入队
- **单消费者**：postmaster 的 accept 循环出队 `PMChild*`
- **同步机制**：使用原子操作（`pg_atomic_*`）实现无锁同步
- **队列实现**：共享内存中的循环缓冲区

### 3. 队列容量

- **容量大小**：队列容量等于 `MaxBackends`
- **固定大小**：在启动时确定，基于 `MaxBackends` 计算
- **共享内存**：队列存储在共享内存中，所有进程可访问

### 4. 集成点

- **后端启动**：后端进程在 `BackendStartupData` 中接收 `PMChild*`
- **连接结束**：后端进程在 `PostgresMain` 中收到 `Terminate` 或 `EOF` 时调用 `PoolEnqueuePMChild()`
- **等待重用**：后端进程入队后使用 `WaitLatch` 等待 postmaster 分配新连接
- **获取空闲后端**：postmaster 在 `ServerLoop()` 中调用 `PoolGetIdleBackend()` 获取空闲后端
- **过期清理**：postmaster 定期调用 `PoolCleanupExpired()` 清理过期后端

## GUC 参数

### 1. `enable_connection_pool` (PGC_POSTMASTER)

**类型**: `bool`  
**默认值**: `false`  
**上下文**: `PGC_POSTMASTER` (需要重启服务器)

启用或禁用内置连接池。此参数必须在服务器启动时设置，修改后需要重启 PostgreSQL 才能生效。

**配置方式：**

```conf
# postgresql.conf
enable_connection_pool = on
```

或通过命令行启动：
```bash
postgres -c enable_connection_pool=on
```

### 2. `connection_pool_size` (PGC_SIGHUP)

**类型**: `int`  
**默认值**: `10`  
**上下文**: `PGC_SIGHUP` (可通过 SIGHUP 动态调整)  
**范围**: `1` 到 `MAX_BACKENDS`

设置连接池中可保留的最大后端进程数。此参数可以在运行时通过重新加载配置文件来调整，无需重启服务器。

**配置方式：**

```conf
# postgresql.conf
connection_pool_size = 20
```

动态调整：
```sql
ALTER SYSTEM SET connection_pool_size = 20;
SELECT pg_reload_conf();
```

### 3. `connection_pool_idle_timeout` (PGC_SIGHUP)

**类型**: `int`  
**默认值**: `300` (5分钟)  
**上下文**: `PGC_SIGHUP` (可通过 SIGHUP 动态调整)  
**范围**: `0` 到 `INT_MAX`  
**单位**: 秒

设置连接池中空闲后端进程的超时时间。超过此时间未使用的后端进程将被终止。

- `0` 表示禁用超时，空闲后端将一直保留在池中
- 大于 `0` 的值表示空闲超时时间（秒）

**配置方式：**

```conf
# postgresql.conf
connection_pool_idle_timeout = 600  # 10分钟
```

动态调整：
```sql
ALTER SYSTEM SET connection_pool_idle_timeout = 600;
SELECT pg_reload_conf();
```

## 工作流程

### 1. 后端进程入队

当后端进程完成客户端连接（收到 `Terminate` 或 `EOF`）时：

1. 清理当前连接状态（中止事务、重置连接、禁用超时等）
2. 调用 `PoolEnqueuePMChild(MyPMChildPtr)` 将自己入队
3. 使用 `WaitLatch` 等待 postmaster 分配新连接
4. 如果收到 `SIGTERM`，退出进程

### 2. Postmaster 获取空闲后端

在 `ServerLoop()` 中，当有新连接到达时：

1. 调用 `PoolGetIdleBackend()` 尝试从队列中获取空闲后端
2. 如果获取成功，可以重用该后端（当前实现中，连接分配尚未完全实现）
3. 如果队列为空，创建新的后端进程

### 3. 过期清理

`PoolCleanupExpired()` 定期检查队列头部的后端：

1. 使用 `LockFreeSlotQueuePeek()` 查看队列头部（不出队）
2. 检查空闲时间是否超过 `idle_timeout`
3. 如果超时，累计超时计数
4. 连续 3 次超时检查后，出队并发送 `SIGTERM` 终止该后端
5. 从 `ActiveChildList` 中移除并释放 slot

## 配置示例

### 高并发短连接应用

```conf
enable_connection_pool = on
connection_pool_size = 50
connection_pool_idle_timeout = 300
```

适合频繁创建和关闭连接的应用，连接池可以显著减少进程创建开销。

### 低并发长连接应用

```conf
enable_connection_pool = on
connection_pool_size = 10
connection_pool_idle_timeout = 1800  # 30分钟
```

适合连接数较少但连接持续时间较长的应用。

### 开发/测试环境

```conf
enable_connection_pool = on
connection_pool_size = 10
connection_pool_idle_timeout = 0  # 禁用超时
```

开发环境中，可以禁用超时以便调试。

## 动态调整

### 可以动态调整的参数

- ✅ `connection_pool_size` - 可以随时调整
- ✅ `connection_pool_idle_timeout` - 可以随时调整

### 需要重启的参数

- ❌ `enable_connection_pool` - 必须重启才能启用/禁用

### 动态调整的影响

当通过 `pg_reload_conf()` 或 `SIGHUP` 重新加载配置时：

1. **`connection_pool_size` 减小**：
   - 新值立即生效
   - 现有的空闲后端不会被立即终止
   - 当空闲后端超时或被使用时，池大小会自然调整到新值

2. **`connection_pool_size` 增大**：
   - 新值立即生效
   - 可以容纳更多的空闲后端

3. **`connection_pool_idle_timeout` 调整**：
   - 新值立即生效
   - 下次清理过期后端时会使用新的超时时间

## 检查当前配置

```sql
-- 查看所有连接池相关参数
SELECT name, setting, unit, context, source
FROM pg_settings
WHERE name LIKE 'connection_pool%' OR name = 'enable_connection_pool'
ORDER BY name;

-- 查看连接池是否启用
SHOW enable_connection_pool;

-- 查看连接池大小
SHOW connection_pool_size;

-- 查看空闲超时时间
SHOW connection_pool_idle_timeout;
```

## 实现细节

### 共享内存结构

```c
typedef struct ConnectionPoolShmem
{
    LockFreeSlotQueue *queue;        /* 无锁队列 */
    int         max_pool_size;       /* 最大池大小（来自 GUC） */
    int         current_pool_size;   /* 当前池中的后端数量 */
    int         idle_timeout;        /* 空闲超时时间（来自 GUC） */
} ConnectionPoolShmem;
```

### 队列条目

```c
typedef struct PoolSlotEntry {
    pg_atomic_uint64 seq;         /* 槽位序号：协调并发读写与状态 */
    pg_atomic_uint64 pmchild_ptr; /* PMChild* 作为地址值（0 表示空） */
    TimestampTz idle_since;       /* 后端变为空闲的时间戳 */
} PoolSlotEntry;
```

`idle_since` 不使用原子类型的前提是：消费者只有在观察到“就绪态”的 `seq` 之后才读取它，且 `seq` 的读写使用了合适的内存屏障语义，从而把 `idle_since` 的写入可见性一并“带出来”。

### 关键函数

- `PoolEnqueuePMChild()`: 后端进程入队（生产者）
- `PoolGetIdleBackend()`: Postmaster 出队（消费者）
- `PoolCleanupExpired()`: 清理过期后端
- `LockFreeSlotQueuePeek()`: 查看队列头部（不出队）

## 注意事项

1. **启用连接池需要重启**：`enable_connection_pool` 是 `PGC_POSTMASTER` 级别，修改后必须重启 PostgreSQL。

2. **队列容量固定**：队列容量在启动时确定，等于 `MaxBackends`，不能动态调整。

3. **进程重用**：后端进程在完成连接后不会退出，而是入队等待重用，减少进程创建开销。

4. **过期清理**：使用连续超时检查机制，避免频繁清理，提高稳定性。

5. **等待机制**：后端进程使用 `WaitLatch` 等待，可以被 postmaster 立即唤醒，响应更快。

6. **连接分配**：当前实现中，从池中获取空闲后端后，连接分配机制尚未完全实现（TODO）。

## 性能考虑

- **无锁设计**：使用原子操作，避免锁竞争，提高并发性能
- **直接指针访问**：出队时直接返回 `PMChild*`，无需查找
- **批量清理**：使用连续超时检查，避免频繁清理操作
- **等待优化**：使用 `WaitLatch` 替代轮询，减少 CPU 占用

## 限制

1. **队列容量**：队列容量等于 `MaxBackends`，不能超过
2. **连接分配**：当前实现中，从池中获取空闲后端后，连接分配机制尚未完全实现
3. **动态扩容**：队列容量在启动时固定，不能动态增加（需要重启）

## 未来改进

1. **连接分配**：实现将新连接分配给已存在的后端进程的机制
2. **动态扩容**：支持动态增加队列容量（需要重新分配共享内存）
3. **统计信息**：提供更详细的连接池统计信息
