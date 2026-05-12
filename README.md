# 基于 PostgreSQL 自定义连接池优化

## 一、项目简介

本项目基于 PostgreSQL 官方源码（上游：`postgres/postgres`）进行二次开发，在 `conn_pool` 分支中实现了服务端自定义连接池机制。目标：减少频繁创建/销毁数据库连接带来的性能开销，提升高并发场景下的系统吞吐量。

项目包含完整的改造源码、编译配置、测试模块及容器化部署文件，满足毕业设计答辩与成果归档要求。

## 二、环境依赖

- **操作系统**：Ubuntu 22.04（手动编译部署推荐）
- **容器环境**：Docker（使用容器化部署时需要）
- **编译依赖**（手动部署）：`git gcc make pkg-config libicu-dev bison flex libreadline-dev zlib1g-dev`
- **测试依赖**：Python 3

## 三、仓库结构

仓库根目录即为 PostgreSQL 源码根目录，包含以下额外内容：

| 文件/目录 | 说明 |
|----------|------|
| `Dockerfile` | 容器镜像构建文件 |
| `test/` | 连接池功能测试模块（含测试脚本与用例） |
| `README.md` | 本文件 |

其余文件为 PostgreSQL 原生源码及 `conn_pool` 分支的改造代码。

## 四、部署方式

### 方式一：手动编译部署（Ubuntu 22.04）
```bash
# 1. 安装编译依赖
sudo apt update && sudo apt install -y git gcc make pkg-config libicu-dev bison flex libreadline-dev zlib1g-dev

# 2. 创建 postgres 用户（及 home 目录）
sudo useradd -m postgres
sudo passwd -d postgres   # 可选：清空密码，便于本地免密切换

# 3. 克隆源码（可以放在任意目录，例如 /tmp 或当前用户目录下）
git clone https://github.com/evtrouble/postgres.git -b conn_pool
cd postgres

# 4. 配置并编译（编译本身不需要 postgres 用户，但安装路径会在 postgres 的 home 下）
#    先确保 /home/postgres 存在且后续安装时能用 sudo 改权限
./configure --prefix=/home/postgres/pg_install --enable-debug CFLAGS=-O0
make -j$(nproc)
sudo make install   # 安装到 /home/postgres/pg_install，需要 sudo 创建目录

# 5. 将安装目录的所有权交给 postgres 用户
sudo chown -R postgres:postgres /home/postgres

# 6. 切换到 postgres 用户并初始化数据库
sudo su - postgres
initdb -D /home/postgres/pgdata --auth-local=trust --username=postgres

# 7. 启动数据库
pg_ctl -D /home/postgres/pgdata -l logfile start

# 8. 配置 PATH 并登录
echo "export PATH=/home/postgres/pg_install/bin:\$PATH" >> ~/.bashrc
source ~/.bashrc
psql
```

### 方式二：Docker 容器化部署
```bash
# 构建镜像（在项目根目录执行）
docker build -t pg-conn-pool-demo .

# 运行容器（后台运行）
docker run -d --name pg-demo pg-conn-pool-demo

# 进入容器终端
docker exec -it pg-demo bash

# 在容器内，启动 PostgreSQL 服务
pg_ctl start

# 此时可以运行 psql 登录数据库
psql

# 测试完成后，停止并删除容器（可选）
docker stop pg-demo && docker rm pg-demo
```

## 五、测试验证
详细的连接池功能测试与性能测试说明请参见 [test/README.md](test/README.md)。

快速执行（确保数据库服务已启动）：

```bash
cd test
python3 test_pool.py
```
性能测试（只读与 TPC-B 脚本）的使用方法请参考 test/README.md 中的对应章节。

## 六、官方回归测试
PostgreSQL 源码自带完整的回归测试套件，用于验证数据库核心功能是否正确。编译完成后（在源码根目录下），执行以下命令：

```bash
# 切换到源码目录（例如容器内 /home/postgres/postgres）
cd /home/postgres/postgres

# 运行回归测试（会自动管理临时实例，无需预先启动数据库）
make check
```

回归测试将运行约 230 个测试用例，预期输出末尾包含：
```text
All 230 tests passed.
```