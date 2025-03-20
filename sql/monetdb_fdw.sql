-- !!! If you want to run regression tests, you need to create a database named "test" in MonetDB with the default port 50000.

CREATE EXTENSION monetdb_fdw;

CREATE SERVER foreign_server FOREIGN DATA WRAPPER monetdb_fdw
OPTIONS (host '127.0.0.1', port '50000', dbname 'test');

CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'monetdb', password 'monetdb');

SELECT monetdb_execute('foreign_server', $$DROP USER test_u$$);
SELECT monetdb_execute('foreign_server', $$DROP SCHEMA test_u$$);
SELECT monetdb_execute('foreign_server', $$CREATE USER "test_u" WITH PASSWORD 'test_u' NAME 'test_user' SCHEMA "sys"$$);
SELECT monetdb_execute('foreign_server', $$CREATE SCHEMA IF NOT EXISTS "test_u" AUTHORIZATION "test_u"$$);
SELECT monetdb_execute('foreign_server', $$ALTER USER "test_u" SET SCHEMA "test_u"$$);

DROP USER MAPPING FOR CURRENT_USER SERVER foreign_server;
CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'test_u', password 'test_u');

SELECT monetdb_execute('foreign_server', $$CREATE TABLE emp( name VARCHAR(20), age INTEGER)$$);

CREATE FOREIGN TABLE emp(
        name VARCHAR(20),
        age INTEGER
)
SERVER foreign_server
OPTIONS (schema_name 'test_u', table_name 'emp');

\des
\d+ emp

-- test insert 
INSERT INTO emp VALUES('John', 23);
INSERT INTO emp VALUES('Mary', 22);

-- test select
SELECT * FROM emp;
SELECT monetdb_execute('foreign_server', $$SELECT * FROM emp$$);

-- test explain
-- EXPLAIN (COSTS OFF) SELECT * FROM emp;
EXPLAIN (COSTS OFF) INSERT INTO emp VALUES('Mary test', 21);

-- test truncate
TRUNCATE emp;
SELECT * FROM emp;
INSERT INTO emp VALUES('John', 23);
INSERT INTO emp VALUES('Mary', 22);
TRUNCATE TABLE emp;
SELECT * FROM emp;

SELECT monetdb_execute('foreign_server', $$CREATE TABLE delete_emp(name VARCHAR(20) PRIMARY KEY, age INTEGER)$$);
CREATE FOREIGN TABLE delete_emp(
        name VARCHAR(20),
        age INTEGER
)
SERVER foreign_server
OPTIONS (schema_name 'test_u', table_name 'delete_emp');

INSERT INTO delete_emp VALUES('John', 23);
INSERT INTO delete_emp VALUES('Mary', 22);
SELECT * FROM delete_emp;

INSERT INTO delete_emp VALUES('Mary', 22); -- error
INSERT INTO delete_emp VALUES('Mary2', 22);
SELECT * FROM delete_emp;

-- test delete
DELETE FROM delete_emp WHERE name = 'John'; -- error, need set key
ALTER FOREIGN TABLE delete_emp ALTER name OPTIONS (ADD key 'true');
DELETE FROM delete_emp WHERE name = 'John';
SELECT * FROM delete_emp;
DELETE FROM delete_emp WHERE age = 22;
SELECT * FROM delete_emp;

set client_min_messages = 'debug2';
INSERT INTO emp VALUES('John', 23);
INSERT INTO emp VALUES('Mary', 22);
SELECT * FROM emp;

INSERT INTO delete_emp VALUES('John', 23);
INSERT INTO delete_emp VALUES('Mary', 22);
SELECT * FROM delete_emp;

-- test update
UPDATE emp SET name = 'Mary2' WHERE name = 'Mary'; -- error
UPDATE delete_emp SET name = 'Mary2' WHERE name = 'Mary'; -- ok
SELECT * FROM delete_emp;
UPDATE delete_emp SET name = 'John2' WHERE age = 23; 
SELECT * FROM delete_emp;

-- test returning
INSERT INTO delete_emp VALUES('John', 23) RETURNING *;
DELETE FROM delete_emp WHERE name = 'John' RETURNING *;
SELECT * FROM delete_emp;
-- UPDATE delete_emp SET name = 'Mary' WHERE name = 'Mary2' RETURNING *; -- error, MonetDB "UPDATE ... RETURNING" has bug
SELECT * FROM delete_emp;

DROP FOREIGN TABLE emp;
DROP FOREIGN TABLE delete_emp;
SELECT monetdb_execute('foreign_server', $$CREATE TABLE test_default(name VARCHAR(20) default 'zm', age INTEGER)$$);
-- test IMPORT FOREIGN SCHEMA
IMPORT FOREIGN SCHEMA "test_u" from server foreign_server into public;
\dE
SELECT * FROM emp;
SELECT * FROM delete_emp;
EXPLAIN VERBOSE INSERT INTO test_default(age) values(22);
INSERT INTO test_default(age) values(22);
SELECT * FROM test_default;
DROP FOREIGN TABLE test_default;

-- test IMPORT FOREIGN SCHEMA ... LIMIT TO
IMPORT FOREIGN SCHEMA "test_u" limit to (test_default) from server foreign_server into public;
\d+ test_default
SELECT * FROM test_default;

-- test the joint primary key 
SELECT monetdb_execute('foreign_server', $$CREATE TABLE orders (
    order_id NUMERIC,
    product_id NUMERIC,
    customer_email VARCHAR(100),
    order_date DATE,
    quantity NUMERIC,
    CONSTRAINT pk_orders PRIMARY KEY (order_id, product_id)
)$$);

IMPORT FOREIGN SCHEMA "test_u" limit to (orders) from server foreign_server into public;
\dE+ orders
\d+ orders

INSERT INTO orders (order_id, product_id, customer_email, order_date, quantity)
VALUES (1, 101, 'john.doe@example.com', '2025-01-01', 5);
INSERT INTO orders (order_id, product_id, customer_email, order_date, quantity)
VALUES (2, 102, 'jane.smith@example.com', '2025-02-15', 3);

UPDATE orders
SET quantity = 10
WHERE order_id = 1 AND product_id = 101;

DELETE FROM orders
WHERE order_id = 2 AND product_id = 102;

SELECT * FROM orders; 

set client_min_messages = 'INFO';
DROP FOREIGN TABLE emp;
DROP FOREIGN TABLE delete_emp;
DROP FOREIGN TABLE test_default;
DROP FOREIGN TABLE orders;
SELECT monetdb_execute('foreign_server', $$DROP TABLE orders$$);
SELECT monetdb_execute('foreign_server', $$DROP TABLE test_default$$);
SELECT monetdb_execute('foreign_server', $$DROP TABLE delete_emp$$);
SELECT monetdb_execute('foreign_server', $$DROP TABLE emp$$);