[简体中文版](README_cn.md)

## MonetDB_fdw

MonetDB_fdw is a PostgreSQL extension based on Foreign Data Wrapper (FDW) technology, which can enhance PostgreSQL analytical capabilities.

The work based on the excellent oracle_fdw (https://github.com/laurenz/oracle_fdw.git) & postgres_fdw (https://www.postgresql.org/docs/current/postgres-fdw.html) projects.

### Supported OS & Database Versions

* RHEL 8/9, CentOS 8/9
* Halo 1.0.14, 1.0.16
* PostgreSQL 14, 15, 16
* MonetDB 11.54

### Cookbook

#### Installation

* Build as PGXS

```sh
export USE_PGXS=1
export MONETDB_HOME=<MonetDB installation path>
export PATH=$MONETDB_HOME/bin:$PATH
export LD_LIBRARY_PATH=$MONETDB_HOME/lib64:$LD_LIBRARY_PATH
git clone https://github.com/HaloLab001/MonetDB_fdw.git
cd MonetDB_fdw
make && make install
```

* Build in a source tree of PostgreSQL

```sh
export MONETDB_HOME=<MonetDB installation path>
export PATH=$MONETDB_HOME/bin:$PATH
export LD_LIBRARY_PATH=$MONETDB_HOME/lib64:$LD_LIBRARY_PATH
git clone https://github.com/HaloLab001/MonetDB_fdw.git <PostgreSQL contrib source path>
cd <PostgreSQL contrib source path>/MonetDB_fdw
make && make install
```

#### Quick Tutorial

* Create MonetDB_fdw extension

```sql
CREATE EXTENSION monetdb_fdw;
```

* Create foreign server

```sql
CREATE SERVER foreign_server FOREIGN DATA WRAPPER monetdb_fdw
OPTIONS (host '127.0.0.1', port '50000', dbname 'test');
```

* Create user mapping

```sql
CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'zm', password 'zm');
```

* Create table (emp for a example) in MonetDB using monetdb_execute function

```sql
SELECT monetdb_execute('foreign_server', $$CREATE TABLE emp(
        name VARCHAR(20),
        age INTEGER
)$$);
```

* Create foreign table

```sql
CREATE FOREIGN TABLE emp(
        name VARCHAR(20),
        age INTEGER
)
SERVER foreign_server
OPTIONS (schema_name 'zm', table_name 'emp');
```

* Now you can query the MonetDB emp table in PostgreSQL

```sql
SELECT count(*) FROM emp;
```

* NOTE: you can IMPORT FOREIGN SCHEMA to create foreign table for convenient

```sql
DROP FOREIGN TABLE emp;
IMPORT FOREIGN SCHEMA "zm" limit to (emp) from server foreign_server into public;
```

#### Supported Operations

* INSERT
* DELETE
* UPDATE
* SELECT
* COPY
* TRUNCATE
* EXPLAIN
* IMPORT FOREIGN SCHEMA

Be careful!!! There is a crash bug of UPDATE ... RETURNING in MonetDB (https://github.com/MonetDB/MonetDB/issues/7623) if table has PK.

#### Supported Types


| Type                         | Supported | Description                                                                                                               |
| ---------------------------- | --------- | ------------------------------------------------------------------------------------------------------------------------- |
| CHAR                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| VARCHAR                      | Y         | Ref PostgreSQL Doc                                                                                                        |
| TEXT                         | Y         | Ref PostgreSQL Doc. TEXT(x) is not supported，<br />TEXT(x) will transform to VARCHAR(x) when imported into PostgreSQL    |
| CLOB                         | Y         | Base type is TEXT. CLOB(x) is not supported，<br />CLOB(x) will transform to VARCHAR(x) when imported into PostgreSQL     |
| STRING                       | Y         | Base type is TEXT，STRING(x) is not supported，<br />STRING(x) will transform to VARCHAR(x) when imported into PostgreSQL |
| BLOB                         | N         |                                                                                                                           |
| BOOL                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| TINYINT                      | Y         | Base type is smallint                                                                                                     |
| SMALLINT                     | Y         | Ref PostgreSQL Doc                                                                                                        |
| INTEGER                      | Y         | Ref PostgreSQL Doc                                                                                                        |
| BIGINT                       | Y         | Ref PostgreSQL Doc                                                                                                        |
| HUGEINT                      | N         | Will support soon                                                                                                         |
| DECIMAL                      | Y         | NUMERIC                                                                                                                   |
| REAL                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| DOUBLE PRECISION             | Y         | Ref PostgreSQL Doc                                                                                                        |
| FLOAT                        | Y         | Ref PostgreSQL Doc                                                                                                        |
| DATE                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| TIME                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| TIME WITH TIME ZONE          | Y         | Ref PostgreSQL Doc                                                                                                        |
| TIMESTAMP                    | Y         | Ref PostgreSQL Doc                                                                                                        |
| TIMESTAMP WITH TIME ZONE     | Y         | Ref PostgreSQL Doc                                                                                                        |
| INTERVAL interval\_qualifier | NoT       |                                                                                                                           |
| JSON                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| UUID                         | Y         | Ref PostgreSQL Doc                                                                                                        |
| URL                          | Y         | Base type is TEXT                                                                                                         |
| INET                         | Y         | Ref PostgreSQL Doc                                                                                                        |

Test case please reference [type\_support.sql](./sql/type_support.sql)

#### Limits

Primary Key is required for DELETE and UPDATE operations.
