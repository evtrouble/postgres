# PostgreSQL 内置连接池测试脚本

此目录包含用于验证 PostgreSQL 内置连接池复用性和隔离性的自动化测试脚本。

## 功能

`test_pool.py` 脚本执行以下“污染-复用-验证”流程：

1.  **Session A (污染者)**:
    *   连接数据库
    *   修改 GUC (`work_mem`)
    *   创建临时表
    *   创建 Prepared Statement
    *   获取后端 PID
    *   断开连接

2.  **Session B (复用者)**:
    *   连接数据库
    *   获取后端 PID（验证是否复用了 Session A 的进程）
    *   检查 GUC 是否重置
    *   检查临时表是否已清除
    *   检查 Prepared Statement 是否已清除

## 使用方法

### 前置条件
*   PostgreSQL 服务已启动。
*   当前环境有 `psql` 可执行文件（如果在源码目录，脚本会自动尝试查找 `src/bin/psql/psql`）。
*   默认连接 `postgres` 数据库，用户为当前系统用户。

### 运行测试

```bash
python3 test_pool.py
```

### 配置

如果需要连接到特定的主机或端口，或者使用了特定的数据库名，请编辑 `test_pool.py` 文件顶部的配置部分：

```python
DB_NAME = "postgres"
DB_USER = "your_username"
# DB_HOST = "localhost"
# DB_PORT = "5432"
```

或者通过环境变量传递 `psql` 路径：

```bash
PSQL_PATH=/path/to/your/psql python3 test_pool.py
```
