# PostgreSQL 连接池测试模块

本目录包含针对 `conn_pool` 分支自定义连接池的**功能测试**与**性能测试**。

## 目录结构
```text
test/
├── test_pool.py # 功能测试入口脚本（推荐）
├── pooltests/ # 功能测试框架
│ 	├── common.py
│ 	├── runner.py
│ 	└── cases/ # 测试用例
│ 		├── test_basic_isolation.py
│ 		├── test_role_auth_isolation.py
│ 		├── test_uncommitted_lock_cleanup.py
│ 		├── test_error_cleanup.py
│ 		└── test_many_cycles_rss.py
├── compare_connection_modes.sh # 只读性能测试脚本
├── compare_tpcb.sh # TPC-B 读写混合性能测试脚本
└── README.md # 本文件
```

---

## 一、功能测试

功能测试用于验证连接池的核心特性：会话隔离、锁清理、错误恢复、内存稳定性等。
> **默认数据库**：所有功能测试默认在 `postgres` 数据库中执行，测试框架会自动创建临时数据库 `pooltest_db` 并清理。

### 1.1 运行前提

- PostgreSQL 服务已启动（容器内需手动执行 `pg_ctl start`）
- Python 3 可用（无需额外依赖，仅标准库）
- 当前用户为 `postgres`（或具有数据库访问权限）

### 1.2 执行全部功能测试

```bash
cd /path/to/postgres/test
python3 test_pool.py
```

### 1.3 预期输出（全部通过）
```text
test_uncommitted_lock_cleanup.py: 通过
test_basic_isolation.py.py: 通过
test_uncommitted_lock_cleanup.py: 通过
test_error_cleanup.py: 通过
test_many_cycles_rss.py: 通过

全部测试通过
```
若某个用例失败，会显示具体错误信息。
### 1.4 重要说明

- **不能直接运行 `pooltests/cases/` 下的单个用例文件**，因为它们仅定义 `run(ctx)` 函数，没有入口。必须通过 `test_pool.py` 或 `pooltests/runner.py` 执行。
- 测试会自动创建临时数据库 `pooltest_db`，结束后自动清理。
- **关于连接复用与 PID 的一致性**：所有功能测试均依赖连接池成功复用后端进程（即前后两次请求获得的 `pg_backend_pid()` 相同）。但在某些情况下（例如数据库刚启动、刚执行完 `make check`、系统负载剧烈波动等），连接池的复用逻辑可能暂时无法保持同一 PID，导致测试失败，输出类似：
```text
test_basic_isolation.py: 失败 1 项
连接未复用（PID 不同）
```
或 `test_error_cleanup.py` 中出现 `ERROR 场景后连接未复用`。  
**解决方法**：通常情况下，重启 PostgreSQL 服务（`pg_ctl restart`）即可恢复正常的复用行为，重新运行测试即可通过。

### 1.5 测试用例说明
|用例文件|测试目的|
|---|---|
|test_basic_isolation.py|验证连接复用时，GUC、临时表、预备语句、游标、LISTEN、advisory lock 被正确重置|
|test_role_auth_isolation.py|验证角色切换后，连接重用时自动恢复原始角色|
|test_uncommitted_lock_cleanup.py|验证未提交的行锁或表锁在连接复用时被强制释放|
|test_error_cleanup.py|验证 SQL 错误后，连接池能恢复连接的健康状态|
|test_many_cycles_rss.py|反复获取/释放连接，监测 RSS 和内存上下文，检测内存泄漏|

## 二、性能测试
性能测试通过 pgbench 模拟并发负载，对比四种连接模式的 TPS 与延迟：

|模式|说明|
|---|---|
|Mode 1|无连接池，长连接（pgbench 默认，不复用事务中的连接）|
|Mode 2|无连接池，短连接（pgbench -C，每个事务新建连接）|
|Mode 3|通过 pgBouncer 连接池（短连接，Unix socket）|
|Mode 4|内置连接池（短连接，Unix socket）|

所有连接均使用 Unix socket，避免高并发下 TCP 端口耗尽。默认数据库：所有性能测试均在 postgres 数据库上运行（数据初始化时会创建 pgbench 表）。

### 2.1 前置依赖
- PostgreSQL 已安装且 Unix socket 目录正确（默认 /tmp）
- pgBouncer 已安装并配置为 systemd 服务（服务名 pgbouncer），Unix socket 目录为 /var/run/postgresql
- 当前用户有 sudo 权限执行：
  - sudo systemctl start/stop pgbouncer
  - sudo -u postgres pg_ctl restart（或直接以 postgres 用户运行）
- 环境变量 PGDATA 已设置（例如 export PGDATA=/home/postgres/pgdata）
- pgbench 和 psql 命令可用

### 2.2 pgBouncer 配置
编辑 /etc/pgbouncer/pgbouncer.ini，根据测试场景选择连接池模式：

事务级池

```ini
pool_mode = transaction
;server_reset_query = DISCARD ALL   # 事务级可注释
default_pool_size = 32
```
会话级池：

```ini
pool_mode = session
server_reset_query = DISCARD ALL
default_pool_size = 32
```
修改后执行：

```bash
sudo systemctl restart pgbouncer
```
说明：server_reset_query = DISCARD ALL 可强制重置会话状态，避免状态污染。

### 2.3 只读性能测试（compare_connection_modes.sh）
该脚本执行 pgbench -S（只读查询），数据初始化一次即可。

运行方式：
```bash
chmod +x compare_connection_modes.sh
./compare_connection_modes.sh [--clients N] [--duration N] [--scale N] [--skip-mode {1,2,3,4,all}]
```

示例：
```bash
./compare_connection_modes.sh --clients 8 --duration 30
```

### 2.4 TPC-B 读写混合性能测试（compare_tpcb.sh）
该脚本执行完整的 TPC-B 事务（包含 UPDATE/INSERT），每个模式测试前会重新初始化 pgbench 数据，确保公平性。

运行方式：
```bash
chmod +x compare_tpcb.sh
./compare_tpcb.sh [--clients N] [--duration N] [--scale N] [--skip-mode {1,2,3,4,all}]
```

示例：
```bash
./compare_tpcb.sh --clients 8 --duration 30
```

### 2.5 输出结果
每个测试会在当前目录生成 results_* 或 results_tpcb_* 目录，包含：
- mode_1.txt ~ mode_4.txt：各模式的详细 pgbench 输出
- summary.txt：汇总 TPS 和延迟
- comparison.txt：可读性良好的对比报告

## 三、连接池参数
内置连接池提供以下 GUC 参数。目前所有参数（包括标记为 `PGC_SIGHUP` 的参数）均**不支持 `pg_reload_conf()` 热加载**，修改后必须重启 PostgreSQL 才能生效。可以使用 `ALTER SYSTEM` 将参数写入配置文件，然后重启。

|参数名|类型|默认值|说明
|---|---|---|---|
|enable_connection_pool|bool|true|开启/关闭内置连接池|
|connection_pool_size|int|32|池中最大后端进程数。建议 ≥ 并发客户端数|
|connection_pool_min_idle_size|int|0|池中最小空闲后端进程数|
|connection_pool_idle_timeout|int|30|空闲后端超时（秒），0 表示永不超时|
以设置连接池大小为 50 为例：

```sql
-- 使用 ALTER SYSTEM 写入配置
ALTER SYSTEM SET connection_pool_size = 50;
ALTER SYSTEM SET connection_pool_min_idle_size = 10;
ALTER SYSTEM SET connection_pool_idle_timeout = 60;

-- 重启数据库（必须重启，不可用 pg_reload_conf()）
-- 在 shell 中执行：
-- pg_ctl restart -D $PGDATA -l logfile
```
注意：性能测试脚本中的 set_pool_enabled 函数已经通过 ALTER SYSTEM + 重启的方式自动控制 enable_connection_pool。对于其他参数，建议在运行测试前手动配置并重启数据库。

## 四、常见错误及解决办法
|错误信息|可能原因|解决方法|
|---|---|---|
|Error: Cannot connect to PostgreSQL via Unix socket (/tmp, port 5432)|PostgreSQL 服务未启动|执行 pg_ctl start，或检查 socket 路径是否正确|
|Error: pgbench not found in PATH|未安装 PostgreSQL 客户端工具|安装 postgresql-client 或确认 pgbench 已编译
|Error: pg_ctl not found in PATH|PostgreSQL 安装路径未加入 PATH|设置 PATH=$PATH:/home/postgres/pg_install/bin
|Error: Cannot connect to pgBouncer ...|pgBouncer 未启动或 socket 路径错误|启动 pgBouncer：sudo systemctl start pgbouncer，检查配置中的 unix_socket_dir|