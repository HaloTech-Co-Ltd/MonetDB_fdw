[English version](README.md)

## monetdb_fdw

适用于 MonetDB的PostgreSQL外部数据包装器。

#### 编译和运行环境

* 项目支持 ``Linux``操作系统（其余系统待测试）。
* 项目支持 ``PostgreSQL``16版本（其余版本待测试，预计像pg_duckdb一样，支持14以上的PostgreSQL版本）。
* 项目需要本地安装部署了MonetDB，需要设置环境变量MONETDB_HOME、PATH和LD_LIBRARY_PATH。
  ```sh
  # 可以考虑将这几行放入.bash_profile
  export MONETDB_HOME=您的MonetDB的安装目录
  export PATH=$MONETDB_HOME/bin:$PATH
  export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$MONETDB_HOME/lib64/
  ```

#### 快速开始（Linux）：

```sh
git clone https://github.com/Z-Xiao-M/MonetDB_fdw.git
cd MonetDB_fdw
make && make install
```

#### 一个简单的示例

以下示例的一个前提是相关对象均已存在。

```sql
-- 安装monetdb_fdw插件
CREATE EXTENSION monetdb_fdw;
-- 创建SERVER
CREATE SERVER foreign_server FOREIGN DATA WRAPPER monetdb_fdw
OPTIONS (host '127.0.0.1', port '50000', dbname 'test');
-- 创建USER MAPPING
CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'zm', password 'zm');
-- 我们可以使用monetdb_execute在MonetDB创建或者查询相关对象
-- 这里让我们在MonetDB 创建一张名为emp的表
SELECT monetdb_execute('foreign_server', $$CREATE TABLE emp(
        name VARCHAR(20),
        age INTEGER
)$$);
-- 创建外部表
CREATE FOREIGN TABLE emp(
        name VARCHAR(20),
        age INTEGER
)
SERVER foreign_server
OPTIONS (schema_name 'zm', table_name 'emp');
-- 更推荐使用IMPORT FOREIGN SCHEMA来导入外部表
DROP FOREIGN TABLE emp;
IMPORT FOREIGN SCHEMA "zm" limit to (emp) from server foreign_server into public;
```

#### 支持语句

MonetDB_fdw支持INSERT、DELETE、UPDATE、SELECT、TRUNCATE、EXPLAIN和IMPORT FOREIGN SCHEMA语句。

以及相关的RETURNING语句，有趣的是MonetDB的UPDATE ... RETURNING似乎存在[BUG](https://github.com/MonetDB/MonetDB/issues/7623)，所以请不要使用在系统中UPDATE ... RETURNING。

#### 类型


| 类型名称                     | 是否支持 | 额外描述                                                                                                                  |
| ---------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------------- |
| CHAR                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| VARCHAR                      | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| TEXT                         | 支持     | 请参考PostgreSQL官方文档，不支持TEXT(x)这样的使用方式，<br />在执行IMPORT FOREIGN SCHEMA时，原有的TEXT(x)会变成VARCHAR(x) |
| CLOB                         | 支持     | 本质上是TEXT的DOMAIN，不支持CLOB(x)这样的使用方式，<br />在执行IMPORT FOREIGN SCHEMA时，原有的CLOB(x)会变成VARCHAR(x)     |
| STRING                       | 支持     | 本质上是TEXT的DOMAIN，不支持STRING(x)这样的使用方式，<br />在执行IMPORT FOREIGN SCHEMA时，原有的STRING(x)会变成VARCHAR(x) |
| BLOB                         | 暂未支持 |                                                                                                                           |
| BOOL                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| TINYINT                      | 支持     | 本质上是SMALLINT的DOMAIN，大小范围-127至127                                                                               |
| SMALLINT                     | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| INTEGER                      | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| BIGINT                       | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| HUGEINT                      | 暂未支持 | 一个初步的设想是使用另一个PostgreSQL的插件来是实现支持                                                                    |
| DECIMAL                      | 支持     | 内部均会转换成NUMERIC，请参考PostgreSQL官方文档                                                                           |
| REAL                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| DOUBLE PRECISION             | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| FLOAT                        | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| DATE                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| TIME                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| TIME WITH TIME ZONE          | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| TIMESTAMP                    | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| TIMESTAMP WITH TIME ZONE     | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| INTERVAL interval\_qualifier | 待测试   |                                                                                                                           |
| JSON                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| UUID                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |
| URL                          | 支持     | 本质上是TEXT的DOMAIN                                                                                                      |
| INET                         | 支持     | 请参考PostgreSQL官方文档                                                                                                  |

相关类型测试内容详见[type_support.sql](./sql/type_support.sql)

#### 限制

由于是参考了Oracle_fdw来实现的功能，所以当使用DELETE、UPDATE语句时，要求远端MonetDB的表中存在主键，

同时需要在PostgreSQL中需要将对应的字段相应的标识一下，可以使用如下语句设置

```
ALTER FOREIGN TABLE tab ALTER col OPTIONS (ADD key 'true');
```

使用IMPORT FOREIGN SCHEMA导入外部表会自动标识相关主键字段。

#### 回归测试

如果您想要运行回归测试，这需要您在MonetDB创建一个名为TEST的数据库

IP：127.0.0.1 PORT：50000

相关测试内容详见[monetdb_fdw.sql](./sql/monetdb_fdw.sql)
