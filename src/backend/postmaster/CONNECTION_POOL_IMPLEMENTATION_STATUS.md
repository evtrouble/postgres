# 连接池实现状态

## ✅ 已完成的工作

### 1. 基础框架
- ✅ 创建了 `connection_pool.c` 和 `connection_pool.h`
- ✅ 实现了基本的池管理函数：
  - `InitConnectionPool()` - 初始化连接池
  - `ConfigureConnectionPool()` - 配置池参数
  - `PoolAddBackend()` - 添加后端到池
  - `PoolGetIdleBackend()` - 从池获取空闲后端
  - `PoolRemoveBackend()` - 从池移除后端
  - `PoolCleanupExpired()` - 清理过期后端
  - `PoolGetStats()` - 获取池统计信息

### 2. GUC 参数
- ✅ 添加了三个 GUC 参数：
  - `enable_connection_pool` (bool) - 启用/禁用连接池
  - `connection_pool_size` (int) - 连接池大小
  - `connection_pool_idle_timeout` (int) - 空闲超时时间

### 3. 集成到 postmaster
- ✅ 在 `postmaster.c` 中添加了头文件包含
- ✅ 在 `PostmasterMain()` 中初始化连接池
- ✅ 在 `ServerLoop()` 中集成了连接池逻辑
- ✅ 在 `ServerLoop()` 中添加了定期清理过期后端
- ✅ 在 `Makefile` 中添加了 `connection_pool.o`

### 4. 测试框架
- ✅ 创建了性能对比测试脚本
- ✅ 添加了 TPC-B 正确性验证

## ⏳ 待实现的关键功能

### 1. 连接传递机制（最关键）

**问题：** 如何将新接受的 socket 传递给已存在的后端进程？

**当前状态：** 
```c
// postmaster.c:1738-1750
if (idle_backend != NULL)
{
    // TODO: Implement connection assignment to idle backend.
    // For now, we'll create a new backend
    elog(DEBUG2, "got idle backend from pool, but connection assignment not implemented yet");
    /* Fall through to create new backend for now */
}
```

**需要实现：**
- 建立 postmaster 和后端进程之间的通信通道
- 将新接受的 socket 文件描述符传递给后端
- 后端接收并处理新连接

**可能的实现方案：**

#### 方案 A: Unix Domain Socket 传递（推荐）
```c
// 使用 sendmsg/recvmsg 传递文件描述符
int send_fd(int socket, int fd_to_send);
int recv_fd(int socket);
```

#### 方案 B: 管道通信
```c
// postmaster 通过管道通知后端有新连接
// 后端通过管道接收 socket fd
int pool_pipe[2];  // 每个后端一个管道
```

#### 方案 C: 共享内存 + 信号
```c
// 在共享内存中放置连接信息
// 通过信号通知后端
// 后端从共享内存读取连接信息
```

### 2. 后端生命周期管理

**问题：** 当前后端在事务结束后会退出，无法加入连接池。

**需要改变：**
- 后端在事务结束后，不立即退出
- 通知 postmaster 自己变为空闲
- postmaster 将其加入连接池
- 后端等待新连接分配

**实现位置：**
- `src/backend/tcop/postgres.c` - 后端主循环
- 需要添加空闲状态处理逻辑

### 3. 后端状态同步

**问题：** 如何知道后端何时变为空闲？

**需要实现：**
- 后端在事务结束后发送信号/消息给 postmaster
- postmaster 接收并更新后端状态
- 将后端加入连接池

**可能的实现：**
```c
// 在后端进程中
if (enable_connection_pool && transaction_finished)
{
    // 通知 postmaster 我变为空闲了
    SendPMSignal(PMSIGNAL_BACKEND_IDLE);
    // 等待新连接分配
    WaitForNewConnection();
}
```

### 4. 连接池中的后端管理

**当前问题：**
- `PoolAddBackend()` 在 `CleanupBackend()` 中无法使用，因为后端已经退出
- 需要改变后端生命周期，让后端保持运行状态

**需要实现：**
- 修改后端退出逻辑，支持"空闲但不退出"模式
- 实现后端等待新连接的机制

## 📋 实现优先级

### 高优先级（核心功能）
1. **连接传递机制** - 没有这个，连接池无法工作
2. **后端生命周期管理** - 让后端保持运行而不是退出
3. **后端状态同步** - 后端通知 postmaster 变为空闲

### 中优先级（完善功能）
4. 连接池统计和监控
5. 动态调整池大小
6. 连接池预热

### 低优先级（优化）
7. 连接池负载均衡
8. 连接池健康检查
9. 更复杂的池管理策略

## 🔧 下一步工作

### 步骤 1: 实现连接传递机制

选择方案 A（Unix Domain Socket 传递），因为：
- 标准方法，跨进程传递 fd
- PostgreSQL 已有类似机制
- 性能好，延迟低

需要创建：
- `src/backend/postmaster/connection_transfer.c` - 连接传递实现
- 在 postmaster 和后端之间建立通信通道

### 步骤 2: 修改后端生命周期

在 `src/backend/tcop/postgres.c` 中：
- 检测连接池模式
- 事务结束后，不退出，而是进入空闲等待状态
- 等待 postmaster 分配新连接

### 步骤 3: 实现状态同步

添加新的 PMSignal：
- `PMSIGNAL_BACKEND_IDLE` - 后端变为空闲
- postmaster 接收信号，将后端加入池

## 📝 代码位置总结

### 已修改的文件
1. `src/backend/postmaster/postmaster.c`
   - 添加头文件包含
   - 初始化连接池
   - ServerLoop 中集成连接池逻辑
   - 定期清理过期后端

2. `src/backend/postmaster/Makefile`
   - 添加 `connection_pool.o`

3. `src/backend/utils/misc/guc_tables.c`
   - 添加全局变量声明

4. `src/backend/utils/misc/guc_parameters.dat`
   - 添加 GUC 参数定义

### 新创建的文件
1. `src/backend/postmaster/connection_pool.c` - 连接池实现
2. `src/include/postmaster/connection_pool.h` - 连接池头文件

## ⚠️ 当前限制

1. **连接传递未实现** - 无法将新连接传递给已存在的后端
2. **后端生命周期未改变** - 后端仍会退出，无法加入池
3. **状态同步未实现** - 无法知道后端何时变为空闲

## 🎯 测试建议

即使核心功能未完全实现，也可以测试：
1. 编译是否成功
2. 参数是否生效
3. 连接池初始化是否正常
4. 池管理函数是否正常工作（通过日志）

等核心功能实现后，再运行完整的性能对比测试。

