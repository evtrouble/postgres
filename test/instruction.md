./configure --prefix=/path
sudo yum install -y libicu-devel readline-devel
make
make install

export PGDATA=/data/1/gongyunlong.gyl/pgdata
/data/1/gongyunlong.gyl/opt/postgres/
 bin/pg_ctl -D /data/1/gongyunlong.gyl/pgdata -l logfile start

cd path
bin/initdb -D $PGDATA --auth-local=trust --username=$USER
sudo -u $USER bin/pg_ctl -D $PGDATA -l $PGDATA/logfile start
bin/psql -U $USER -d postgres