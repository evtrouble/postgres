# 连接池性能测试指南

## 测试目标

连接池的主要性能优势：
1. **减少进程创建开销**：复用后端进程，避免频繁 fork()
2. **提高连接建立速度**：跳过进程初始化和认证过程
3. **降低资源消耗**：限制并发后端数量

## 测试方法

### 方法 1：连接建立时间测试

**原理：** 测量建立连接所需的时间，连接池应该显著减少这个时间。

```bash
# 运行测试脚本
chmod +x test_connection_pool.sh
./test_connection_pool.sh connection_time
```

**预期结果：**
- 无连接池：每次连接需要 fork 进程（~1-5ms）
- 有连接池：从池中获取（~0.1-0.5ms）
- **预期改进：** 50-80% 的时间节省

### 方法 2：并发连接测试

**原理：** 测试系统处理大量并发连接的能力。

```bash
./test_connection_pool.sh concurrent
```

**预期结果：**
- 无连接池：每个连接创建新进程，资源消耗大
- 有连接池：复用进程，资源消耗受限于池大小
- **预期改进：** 更稳定的性能，更少的进程数

### 方法 3：吞吐量测试

**原理：** 测量每秒能建立的连接数。

```bash
./test_connection_pool.sh throughput
```

**预期结果：**
- 无连接池：受限于进程创建速度
- 有连接池：受限于池大小和复用速度
- **预期改进：** 2-5倍吞吐量提升（取决于池大小）

### 方法 4：使用 pgbench 进行综合测试

**原理：** 使用 PostgreSQL 标准基准测试工具。

```bash
chmod +x test_pgbench_pool.sh
./test_pgbench_pool.sh --clients 20 --duration 60
```

**关键指标：**
- **tps (transactions per second)**：应该提高
- **latency (平均延迟)**：应该降低
- **连接建立时间**：检查日志中的连接时间

### 方法 5：进程数监控

**原理：** 监控后端进程数量，连接池应该限制进程数。

```bash
# 监控进程数
watch -n 1 'ps aux | grep "[p]ostgres:.*postgres" | wc -l'

# 同时运行连接测试
./test_connection_pool.sh connection_time
```

**预期结果：**
- 无连接池：进程数随连接数线性增长
- 有连接池：进程数受限于 `connection_pool_size`
- **预期改进：** 更稳定的资源使用

## 详细测试步骤

### 步骤 1：准备测试环境

```bash
# 1. 确保 PostgreSQL 运行
pg_ctl status

# 2. 设置测试参数（无连接池）
psql -c "ALTER SYSTEM SET enable_connection_pool = false;"
pg_ctl reload

# 3. 等待配置生效
sleep 2
```

### 步骤 2：运行基准测试（无连接池）

```bash
# 运行连接时间测试
./test_connection_pool.sh connection_time > results_no_pool.txt

# 运行吞吐量测试
./test_connection_pool.sh throughput >> results_no_pool.txt

# 运行 pgbench 测试
./test_pgbench_pool.sh --clients 20 --duration 60 >> results_no_pool.txt
```

### 步骤 3：启用连接池

```bash
# 1. 设置连接池参数
psql -c "ALTER SYSTEM SET enable_connection_pool = true;"
psql -c "ALTER SYSTEM SET connection_pool_size = 10;"
psql -c "ALTER SYSTEM SET connection_pool_idle_timeout = 300;"

# 2. 重启 PostgreSQL（这些参数需要重启）
pg_ctl restart

# 3. 等待启动完成
sleep 5
```

### 步骤 4：运行对比测试（有连接池）

```bash
# 运行相同的测试
./test_connection_pool.sh connection_time > results_with_pool.txt
./test_connection_pool.sh throughput >> results_with_pool.txt
./test_pgbench_pool.sh --clients 20 --duration 60 >> results_with_pool.txt
```

### 步骤 5：对比结果

```bash
echo "=== Connection Time Comparison ==="
echo "Without pool:"
grep "Average time" results_no_pool.txt
echo "With pool:"
grep "Average time" results_with_pool.txt

echo ""
echo "=== Throughput Comparison ==="
echo "Without pool:"
grep "Throughput" results_no_pool.txt
echo "With pool:"
grep "Throughput" results_with_pool.txt
```

## 监控指标

### 1. PostgreSQL 日志中的连接时间

启用连接日志：
```sql
ALTER SYSTEM SET log_connections = on;
ALTER SYSTEM SET log_disconnections = on;
ALTER SYSTEM SET log_connection_timing = on;  -- 如果支持
SELECT pg_reload_conf();
```

查看日志：
```bash
tail -f $PGDATA/log/postgresql-*.log | grep "connection"
```

### 2. 系统资源监控

```bash
# CPU 使用率
top -p $(pgrep -f postgres)

# 内存使用
ps aux | grep postgres | awk '{sum+=$6} END {print sum/1024 " MB"}'

# 进程数
ps aux | grep "[p]ostgres:.*postgres" | wc -l
```

### 3. PostgreSQL 统计信息

```sql
-- 查看当前连接数
SELECT count(*) FROM pg_stat_activity;

-- 查看连接建立时间（如果实现了）
SELECT * FROM pg_stat_database;

-- 查看后端进程信息
SELECT pid, usename, datname, state, backend_start 
FROM pg_stat_activity 
WHERE datname = 'postgres';
```

## 预期性能改进

### 连接建立时间
- **无连接池：** 2-5ms（fork + 初始化）
- **有连接池：** 0.1-0.5ms（从池获取）
- **改进：** 80-90% 时间节省

### 吞吐量
- **无连接池：** 受限于进程创建速度（~200-500 conn/s）
- **有连接池：** 受限于池大小（~1000-5000 conn/s）
- **改进：** 2-10倍提升

### 资源使用
- **无连接池：** 进程数 = 并发连接数
- **有连接池：** 进程数 ≤ connection_pool_size
- **改进：** 更稳定的资源使用

## 注意事项

1. **测试环境一致性**
   - 使用相同的硬件配置
   - 关闭其他应用程序
   - 使用相同的 PostgreSQL 配置（除了连接池参数）

2. **多次测试取平均值**
   - 运行多次测试（至少 3-5 次）
   - 计算平均值和标准差
   - 考虑系统负载波动

3. **测试不同场景**
   - 低并发（1-10 连接）
   - 中等并发（10-50 连接）
   - 高并发（50-200 连接）

4. **监控系统资源**
   - 确保不是 CPU/内存/IO 瓶颈
   - 如果系统资源饱和，测试结果不准确

## 故障排查

如果测试结果不符合预期：

1. **检查连接池是否启用**
   ```sql
   SHOW enable_connection_pool;
   ```

2. **检查池配置**
   ```sql
   SHOW connection_pool_size;
   SHOW connection_pool_idle_timeout;
   ```

3. **检查日志**
   ```bash
   tail -f $PGDATA/log/postgresql-*.log
   ```

4. **验证实现**
   - 确认连接池代码已编译
   - 确认函数被正确调用
   - 检查是否有错误日志

## 高级测试

### 压力测试

```bash
# 使用 Apache Bench 风格的测试
for i in {1..1000}; do
    psql -c "SELECT 1;" &
done
wait
```

### 长时间运行测试

```bash
# 运行 1 小时的压力测试
./test_pgbench_pool.sh --clients 50 --duration 3600
```

### 连接池大小影响测试

```bash
# 测试不同池大小的性能
for size in 5 10 20 50; do
    psql -c "ALTER SYSTEM SET connection_pool_size = $size;"
    pg_ctl restart
    sleep 5
    ./test_pgbench_pool.sh --clients 30 --duration 60 > results_pool_${size}.txt
done
```

