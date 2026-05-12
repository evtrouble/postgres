FROM ubuntu:22.04

# 安装依赖 + Python3
RUN apt update && apt install -y \
    sudo git vim systemctl \
    gcc pkg-config libicu-dev bison flex \
    libreadline-dev zlib1g-dev make \
    python3 python3-pip locales pgbouncer && \
    # 生成 locale 
    locale-gen en_US.UTF-8 && \
    update-locale LANG=en_US.UTF-8 && \
    apt clean

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# 创建用户
RUN useradd -m postgres && \
    usermod -aG sudo postgres && \
    echo "postgres ALL=(ALL) NOPASSWD: ALL" > /etc/sudoers.d/postgres
WORKDIR /home/postgres
USER postgres

# 克隆代码
RUN git clone https://github.com/evtrouble/postgres.git -b conn_pool

# 编译安装
WORKDIR /home/postgres/postgres
RUN CFLAGS=-O0 ./configure --prefix=/home/postgres/pg_install --enable-debug
RUN make -j4
RUN make install

# 全局环境变量
ENV PATH=/home/postgres/pg_install/bin:$PATH
ENV PGDATA=/home/postgres/pgdata

# 初始化数据库
RUN mkdir -p $PGDATA
RUN initdb -D $PGDATA --auth-local=trust --username=postgres

# 写入登录配置
RUN echo "export PATH=$PATH" >> /home/postgres/.profile
RUN echo "export PGDATA=$PGDATA" >> /home/postgres/.profile

# ---------- 添加 pgbouncer 配置文件（仅配置，不自启动） ----------
USER root

# userlist.txt：添加 "postgres" ""（空密码，配合 auth_type=trust）
RUN echo '"postgres" ""' > /etc/pgbouncer/userlist.txt

# pgbouncer.ini
RUN cat > /etc/pgbouncer/pgbouncer.ini <<'EOF'
[databases]
postgres = host=127.0.0.1 port=5432 dbname=postgres user=postgres

[pgbouncer]
pool_mode = transaction
;server_reset_query = DISCARD ALL
default_pool_size = 32

listen_addr = localhost
listen_port = 6432

auth_type = trust
auth_file = /etc/pgbouncer/userlist.txt

logfile = /var/log/postgresql/pgbouncer.log
pidfile = /var/run/postgresql/pgbouncer.pid

unix_socket_dir = /var/run/postgresql
unix_socket_mode = 0777 
EOF

EXPOSE 5432
CMD ["tail", "-f", "/dev/null"]