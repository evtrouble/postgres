# 连接模式性能对比测试指南

## 四种测试模式

### 模式 1: 无池长连接（Baseline - 最佳性能）
**描述：** pgbench 使用持久连接，每个客户端在整个测试期间保持一个连接

**pgbench 参数：**
```bash
pgbench -c 20 -j 20 -T 60 -r postgres
# 不加 -C 参数
```

**特点：**
- ✅ 最佳性能（无连接建立开销）
- ✅ 最低延迟
- ✅ 最稳定的性能
- ❌ 需要保持大量连接

**适用场景：** 作为性能基准

---

### 模式 2: 无池短连接（最差性能 - 连接开销）
**描述：** 每个事务都新建连接，模拟无连接池的 Web 应用

**pgbench 参数：**
```bash
pgbench -c 20 -j 20 -T 60 -C -r postgres
# 加 -C 参数
```

**特点：**
- ❌ 最差性能（每次事务都要 fork 进程）
- ❌ 最高延迟（包含连接建立时间）
- ❌ 高 CPU 开销（频繁 fork）
- ✅ 模拟真实 Web 应用场景

**适用场景：** 对比基准，展示连接池的价值

---

### 模式 3: pgBouncer 连接池（外部代理）
**描述：** 通过 pgBouncer 代理，客户端短连接，pgBouncer 维护长连接池

**pgbench 参数：**
```bash
pgbench -h localhost -p 6432 -c 20 -j 20 -T 60 -C -r postgres
# 连接到 pgBouncer 端口，使用 -C
```

**特点：**
- ✅ 较好的性能（接近长连接）
- ✅ 低延迟（代理开销很小）
- ✅ 成熟的解决方案
- ❌ 需要额外的代理进程
- ❌ 额外的网络跳转

**适用场景：** 对比内置连接池的性能

---

### 模式 4: 内置连接池（你的实现）
**描述：** PostgreSQL 内部复用后端进程，客户端仍使用短连接

**pgbench 参数：**
```bash
pgbench -c 20 -j 20 -T 60 -C -r postgres
# 使用 -C，但 PostgreSQL 内部复用后端
```

**特点：**
- ✅ 应该接近模式 1 的性能
- ✅ 比模式 2 好很多
- ✅ 无需外部代理
- ✅ 更低的延迟（无代理跳转）
- ❌ 需要修改 PostgreSQL 源码

**适用场景：** 验证内置连接池的实现效果

---

## 测试步骤

### 步骤 1: 准备测试环境

```bash
# 1. 确保 PostgreSQL 运行
pg_ctl status

# 2. 初始化 pgbench 数据库
pgbench -i -s 1 postgres

# 3. 检查连接
psql -c "SELECT 1;"
```

### 步骤 2: 运行对比测试

```bash
cd /data/1/gongyunlong.gyl/cpp/postgres/test

# 运行所有模式
./compare_connection_modes.sh --clients 20 --duration 60

# 或者跳过某些模式
./compare_connection_modes.sh --clients 20 --duration 60 --skip-mode 3
```

### 步骤 3: 配置不同模式

#### 模式 1 & 2: 无连接池
```sql
-- 确保连接池关闭
ALTER SYSTEM SET enable_connection_pool = false;
-- 需要重启
SELECT pg_reload_conf();  -- 如果参数支持 reload
```

#### 模式 4: 启用内置连接池
```sql
-- 启用连接池
ALTER SYSTEM SET enable_connection_pool = true;
ALTER SYSTEM SET connection_pool_size = 20;  -- 根据测试调整
ALTER SYSTEM SET connection_pool_idle_timeout = 300;
-- 需要重启
```

### 步骤 4: 设置 pgBouncer（模式 3）

```ini
# /etc/pgbouncer/pgbouncer.ini
[databases]
postgres = host=localhost port=5432 dbname=postgres

[pgbouncer]
listen_port = 6432
listen_addr = localhost
pool_mode = transaction
max_client_conn = 100
default_pool_size = 20
```

```bash
# 启动 pgBouncer
pgbouncer -d /etc/pgbouncer/pgbouncer.ini
```

---

## 预期结果对比

| 模式 | TPS | 延迟 (avg) | 延迟 (stddev) | 说明 |
|------|-----|-----------|--------------|------|
| 1. 长连接 | 基准值 | 最低 | 最低 | 最佳性能 |
| 2. 短连接 | -50~70% | +200~500% | 高 | 连接开销大 |
| 3. pgBouncer | -5~10% | +5~15% | 低 | 接近长连接 |
| 4. 内置池 | -2~5% | +2~10% | 低 | 应该最好 |

### 成功标准

**内置连接池（模式 4）应该：**
1. ✅ TPS 比模式 2 高 50% 以上
2. ✅ 延迟比模式 2 低 50% 以上
3. ✅ TPS 接近模式 1（差距 < 10%）
4. ✅ 延迟接近模式 1（差距 < 15%）
5. ✅ 性能接近或优于模式 3（pgBouncer）

---

## 关键指标解释

### TPS (Transactions Per Second)
- **含义：** 每秒完成的事务数
- **重要性：** 最重要的性能指标
- **期望：** 模式 4 应该接近模式 1

### Latency (Average)
- **含义：** 平均事务延迟
- **重要性：** 用户体验指标
- **期望：** 模式 4 应该接近模式 1

### Latency (Stddev)
- **含义：** 延迟标准差
- **重要性：** 性能稳定性
- **期望：** 模式 4 应该接近模式 1（低波动）

---

## 测试脚本使用

### 基本用法
```bash
./compare_connection_modes.sh --clients 20 --duration 60
```

### 高级选项
```bash
# 指定并发数
./compare_connection_modes.sh --clients 50 --duration 120

# 跳过 pgBouncer 测试
./compare_connection_modes.sh --skip-mode 3

# 只测试内置连接池
./compare_connection_modes.sh --skip-mode 1 --skip-mode 2 --skip-mode 3
```

### 结果文件
测试结果保存在 `results_YYYYMMDD_HHMMSS/` 目录：
- `mode_1.txt` - 模式 1 详细结果
- `mode_2.txt` - 模式 2 详细结果
- `mode_3.txt` - 模式 3 详细结果
- `mode_4.txt` - 模式 4 详细结果
- `summary.txt` - 汇总对比
- `comparison.txt` - 完整对比报告

---

## 故障排查

### 问题 1: 模式 4 性能没有提升
- 检查连接池是否真的启用：`SHOW enable_connection_pool;`
- 检查池大小配置：`SHOW connection_pool_size;`
- 查看 PostgreSQL 日志，确认连接被复用
- 检查连接池代码是否正确集成

### 问题 2: pgBouncer 连接失败
- 检查 pgBouncer 是否运行：`ps aux | grep pgbouncer`
- 检查端口是否正确：`netstat -tlnp | grep 6432`
- 检查 pgBouncer 配置：`cat /etc/pgbouncer/pgbouncer.ini`

### 问题 3: 测试结果不稳定
- 增加测试时长：`--duration 120`
- 多次运行取平均值
- 关闭其他应用程序
- 检查系统负载

---

## 性能分析建议

### 1. 多次测试取平均值
```bash
# 运行 3 次，取平均值
for i in {1..3}; do
    ./compare_connection_modes.sh --clients 20 --duration 60
done
```

### 2. 不同并发数测试
```bash
# 测试不同并发数的影响
for clients in 10 20 50 100; do
    ./compare_connection_modes.sh --clients $clients --duration 60
done
```

### 3. 监控系统资源
```bash
# 在另一个终端监控
watch -n 1 'ps aux | grep postgres | wc -l'
top -p $(pgrep -d, -f postgres)
```

---

## 报告模板

测试完成后，可以生成这样的报告：

```
Connection Pool Performance Comparison Report
=============================================

Test Configuration:
  Date: 2025-01-XX
  Clients: 20
  Duration: 60s
  Scale: 1

Results:
  Mode 1 (Long conn):  TPS=1000, Latency=2.0ms
  Mode 2 (Short conn): TPS=300,  Latency=6.5ms
  Mode 3 (pgBouncer):  TPS=950,  Latency=2.1ms
  Mode 4 (Built-in):   TPS=980,  Latency=2.05ms

Conclusion:
  Built-in connection pool achieves 98% of long connection performance,
  which is 3.3x better than short connections and slightly better than
  pgBouncer (3% improvement).
```

