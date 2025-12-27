[English version](README.md)

## monetdb\_fdw

MonetDB\_fdw 是基于 Foreign Data Wrapper （FDW） 技术的 PostgreSQL 扩展，可以用来增强`PostgreSQL`的分析能力。
本项目基于优秀的`postgres_fdw`[https://www.postgresql.org/docs/current/postgres-fdw.html](https://www.postgresql.org/docs/current/postgres-fdw.html)和 `oracle_fdw`([https://github.com/laurenz/oracle\_fdw.git](https://github.com/laurenz/oracle_fdw.git))项目。

#### 支持的操作系统和数据库

* RHEL 8/9、CentOS 8/9。
* 羲和（Halo）数据库 1.0.14, 1.0.16
* PostgreSQL 14至16版本 （预计将支持14以上的版本）
* MonetDB 11.56（master版本）

#### 源码编译安装

在PGXS上构建

```sh
export USE_PGXS=1
export MONETDB_HOME=<MonetDB installation path>
export PATH=$MONETDB_HOME/bin:$PATH
export LD_LIBRARY_PATH=$MONETDB_HOME/lib64:$LD_LIBRARY_PATH
git clone https://github.com/HaloTech-Co-Ltd/MonetDB_fdw.git
cd MonetDB_fdw
make && make install
```

在PostgreSQL的源代码目录编译安装

```sh
export MONETDB_HOME=<MonetDB installation path>
export PATH=$MONETDB_HOME/bin:$PATH
export LD_LIBRARY_PATH=$MONETDB_HOME/lib64:$LD_LIBRARY_PATH
git clone https://github.com/HaloTech-Co-Ltd/MonetDB_fdw.git <PostgreSQL contrib source path>
cd <PostgreSQL contrib source path>/MonetDB_fdw
make && make install
```

#### 快速上手

* 创建MonetDB_fdw拓展插件

  ```sql
  CREATE EXTENSION monetdb_fdw;
  ```
* 创建外部服务器

  ```sql
  CREATE SERVER foreign_server FOREIGN DATA WRAPPER monetdb_fdw
  OPTIONS (host '127.0.0.1', port '50000', dbname 'test');
  ```
* 创建用户映射

  ```sql
  CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'zm', password 'zm');
  ```
* 在MonetDB中创建一张名为emp的表，这里我们可以使用`monetdb\_execute`帮助我们快速实现

  ```sql
  SELECT monetdb_execute('foreign_server', $$CREATE TABLE emp(
        name VARCHAR(20),
        age INTEGER
  )$$);
  ```
* 创建外部表

  ```sql
  CREATE FOREIGN TABLE emp(
        name VARCHAR(20),
        age INTEGER
  )
  SERVER foreign_server
  OPTIONS (schema_name 'zm', table_name 'emp');
  ```
* 完成上述操作之后，便可以在PostgreSQL中查询MonetDB的emp表中数据了

  ```sql
  SELECT COUNT(*) FROM emp;
  ```
* 一种更为快捷的创建外部表的方法是`IMPORT FOREIGN SCHEMA`([https://www.postgresql.org/docs/current/sql-importforeignschema.html](https://www.postgresql.org/docs/current/sql-importforeignschema.html))

  ```sql
  DROP FOREIGN TABLE emp;
  IMPORT FOREIGN SCHEMA "zm" limit to (emp) from server foreign_server into public;
  ```

####支持语句

* INSERT
* DELETE
* UPDATE
* SELECT
* COPY
* TRUNCATE
* EXPLAIN
* IMPORT FOREIGN SCHEMA。

以及相关的RETURNING语句，有趣的是当前版本的MonetDB在UPDATE ... RETURNING场景暂时还存在[BUG](https://github.com/MonetDB/MonetDB/issues/7623)，所以让我们静待下一个MonetDB版本，请不要在当前系统中使用UPDATE ... RETURNING。

#### 类型


| 类型名称                     | 是否支持 | 额外描述                                                                                                            |
| ---------------------------- | -------- | ------------------------------------------------------------------------------------------------------------------- |
| CHAR                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| VARCHAR                      | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| TEXT                         | 支持     | 请参考PostgreSQL官方文档，不支持TEXT(x)这样的使用方式，在执行IMPORT FOREIGN SCHEMA时，原有的TEXT(x)会变成VARCHAR(x) |
| CLOB                         | 支持     | 本质上是TEXT的DOMAIN，不支持CLOB(x)这样的使用方式，在执行IMPORT FOREIGN SCHEMA时，原有的CLOB(x)会变成VARCHAR(x)     |
| STRING                       | 支持     | 本质上是TEXT的DOMAIN，不支持STRING(x)这样的使用方式，在执行IMPORT FOREIGN SCHEMA时，原有的STRING(x)会变成VARCHAR(x) |
| BLOB                         | 暂未支持 |                                                                                                                     |
| BOOL                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| TINYINT                      | 支持     | 本质上是SMALLINT的DOMAIN，大小范围-127至127                                                                         |
| SMALLINT                     | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| INTEGER                      | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| BIGINT                       | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| HUGEINT                      | 暂未支持 | 一个初步的设想是使用另一个PostgreSQL的插件来是实现支持                                                              |
| DECIMAL                      | 支持     | 内部均会转换成NUMERIC，请参考PostgreSQL官方文档                                                                     |
| REAL                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| DOUBLE PRECISION             | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| FLOAT                        | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| DATE                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| TIME                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| TIME WITH TIME ZONE          | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| TIMESTAMP                    | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| TIMESTAMP WITH TIME ZONE     | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| INTERVAL interval\_qualifier | 待测试   |                                                                                                                     |
| JSON                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| UUID                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |
| URL                          | 支持     | 本质上是TEXT的DOMAIN                                                                                                |
| INET                         | 支持     | 请参考PostgreSQL官方文档                                                                                            |

相关类型测试内容详见[type\_support.sql](./sql/type_support.sql)

#### 限制

由于是参考了oracle\_fdw来实现的功能，所以当使用DELETE、UPDATE语句时，要求远端MonetDB的表中存在主键，

同时需要在PostgreSQL中需要将对应的字段相应的标识一下，可以使用如下语句设置

```
ALTER FOREIGN TABLE tab ALTER col OPTIONS (ADD key 'true');
```

更加推荐使用`IMPORT FOREIGN SCHEMA`，因为它在辅助导入外部表会自动标识相关主键字段。
