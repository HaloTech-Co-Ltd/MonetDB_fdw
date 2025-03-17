[English version](README.md)

## monetdb_fdw

适用于 MonetDB的PostgreSQL外部数据包装器，你可以用它来访问远端MoneDB中的数据。

#### 编译和运行环境

* 项目支持 ``Linux``操作系统（其余系统待测试）。
* 项目需要本地安装部署了MonetDB，需要设置环境变量MONETDB_HOME、PATH和LD_LIBRARY_PATH。
  ```sh
  # 可以考虑将这几行放入.bash_profile
  export MONETDB_HOME=您的MonetDB的安装目录
  export PATH=$MONETDB_HOME/bin:$PATH
  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$MONETDB_HOME/lib64/
  ```

#### 快速开始（Linux）：

```sh
git clone https://github.com/Leo-XM-Zeng/MonetDB_fdw.git
cd MonetDB_fdw
make && make install
```

#### 回归测试

如果您想要运行回归测试，这需要您在MonetDB创建一个名为test的数据库，IP：127.0.0.1 端口50000

相关测试内容详见[monetdb_fdw.sql](./sql/monetdb_fdw.sql)

#### 一个简单的示例

以下示例的一个前提是相关对象均已存在。

```sql
CREATE EXTENSION monetdb_fdw；

CREATE SERVER foreign_server FOREIGN DATA WRAPPER monetdb_fdw
OPTIONS (host '127.0.0.1', port '50000', dbname 'test');

CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'zm', password 'zm');

CREATE FOREIGN TABLE emp(
        name VARCHAR(20),
        age INTEGER
)
SERVER foreign_server
OPTIONS (schema_name 'zm', table_name 'emp');
```

#### 限制

MonetDB_fdw暂时仅支持INSERT、DELETE、SELECT、TRUNCATE和EXPLAIN语句。
当使用DELETE语句时，要求远端MonetDB的表中存在主键，在PostgreSQL中需要将对应的字段相应的设置一下，
可以使用如下语句设置，这是参考了Oracle_fdw来实现的功能，
值得注意的是，MonetDB不支持SELECT FOR UPDATE这种使用方法

```
ALTER FOREIGN TABLE tab ALTER col OPTIONS (ADD key 'true');
```

