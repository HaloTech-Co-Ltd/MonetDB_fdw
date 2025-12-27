-- Set timezone to UTC for consistent test results
SET timezone = 'UTC';

select monetdb_execute('foreign_server', $$CREATE TABLE Numeric_Types(
	a TINYINT,
	b SMALLINT,
	c INTEGER,
	d BIGINT,
	-- e HUGEINT,
	f DECIMAL,
	g NUMERIC(38, 3),
	h REAL,
	i DOUBLE PRECISION,
	j FLOAT
)$$);

IMPORT FOREIGN SCHEMA "test_u" limit to (Numeric_Types) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'numeric_types';

-- test TINYINT
INSERT INTO Numeric_Types(a) VALUES(-127 - 1);
INSERT INTO Numeric_Types(a) VALUES(127);
INSERT INTO Numeric_Types(a) VALUES(127 + 1);
SELECT * FROM Numeric_Types;

TRUNCATE Numeric_Types;
-- test SMALLINT
INSERT INTO Numeric_Types(b) VALUES(-32767 - 1);
INSERT INTO Numeric_Types(b) VALUES(32767);
INSERT INTO Numeric_Types(b) VALUES(32767 + 1);
SELECT * FROM Numeric_Types;

TRUNCATE Numeric_Types;
-- test INTEGER
INSERT INTO Numeric_Types(c) VALUES(-2147483647 - 1);
INSERT INTO Numeric_Types(c) VALUES(2147483647);
INSERT INTO Numeric_Types(c) VALUES(2147483647 + 1);
SELECT * FROM Numeric_Types;

TRUNCATE Numeric_Types;
-- test BIGINT
INSERT INTO Numeric_Types(d) VALUES(-9223372036854775807 - 1);
INSERT INTO Numeric_Types(d) VALUES(9223372036854775807);
INSERT INTO Numeric_Types(d) VALUES(9223372036854775807 + 1);
SELECT * FROM Numeric_Types;

select monetdb_execute('foreign_server', $$CREATE TABLE bool_Types(
	a BOOLEAN
)$$);

IMPORT FOREIGN SCHEMA "test_u" limit to (bool_Types) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'bool_types';

-- test BOOLEAN
INSERT INTO bool_Types VALUES(true);
INSERT INTO bool_Types VALUES('true');
INSERT INTO bool_Types VALUES('1');
INSERT INTO bool_Types VALUES('t');
INSERT INTO bool_Types VALUES(false);
INSERT INTO bool_Types VALUES('false');
INSERT INTO bool_Types VALUES('0');
INSERT INTO bool_Types VALUES('f');
SELECT * FROM bool_Types;

DROP FOREIGN TABLE bool_Types;
DROP FOREIGN TABLE Numeric_Types;
SELECT monetdb_execute('foreign_server', $$DROP TABLE bool_Types$$);
SELECT monetdb_execute('foreign_server', $$DROP TABLE Numeric_Types$$);

-- test CHAR、VARCHAR、CLOB、TEXT、STRING
select monetdb_execute('foreign_server', $$CREATE TABLE Character_Types(
	a CHAR,
	b CHARACTER(10),
	c VARCHAR(20),
	d VARCHAR,
	e CLOB,
	f TEXT,  -- The TEXT type of the external table imported using IMPORT is not the TEXT type of PostgrSQL
	g STRING,
	h CLOB(10),
	i TEXT(20),
	j STRING(30) 
)$$);

IMPORT FOREIGN SCHEMA "test_u" limit to (Character_Types) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'character_types';

-- In fact, CLOB, TEXT, and STRING with precision information are not supported
CREATE TABLE Character_Types2(
	e CLOB,
	f TEXT,
	g STRING
);
DROP TABLE Character_Types2;

-- 
CREATE TABLE Character_Types2(h CLOB(10));      -- error
CREATE TABLE Character_Types2(i TEXT(20));      -- error
CREATE TABLE Character_Types2(j STRING(30));    -- error

DROP FOREIGN TABLE Character_Types;
-- Create an external table with the following PG types
CREATE FOREIGN TABLE Character_Types(
	a CHAR,
	b CHARACTER(10),
	c VARCHAR(20),
	d VARCHAR,
	e CLOB,
	f TEXT,
	g STRING,
	h VARCHAR(10),
	i VARCHAR(20),
	j VARCHAR(30) 
)
SERVER foreign_server
OPTIONS (schema_name 'test_u', table_name 'Character_Types');
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'character_types';
DROP FOREIGN TABLE Character_Types;
SELECT monetdb_execute('foreign_server', $$DROP TABLE Character_Types$$);

-- test TIMESTAMP、TIMESTAMP WITH TIME ZONE、DATE、TIME、TIME WITH TIME ZONE
select monetdb_execute('foreign_server', $$CREATE TABLE Time_Types(
	a TIMESTAMP,
	b TIMESTAMP WITH TIME ZONE,
	c DATE,
	d TIME,
	e TIME WITH TIME ZONE
)$$);

IMPORT FOREIGN SCHEMA "test_u" limit to (Time_Types) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'time_types';

-- Test data insertion
INSERT INTO time_types VALUES('2014-04-24 17:12:12.415', '2014-04-24 17:12:12.415 -02:00', '2014-04-24', '17:12:12.415', '17:12:12.415 -02:00');

-- Verify timestamp types (convert to text for consistent display)
SELECT a::text AS timestamp_without_tz,
       b AT TIME ZONE 'UTC' AS timestamp_with_tz_utc,
       c::text AS date
FROM time_types;

-- Verify time types separately
SELECT d::text AS time_without_tz FROM time_types;
SELECT e AT TIME ZONE 'UTC' AS time_with_tz_utc FROM time_types;
DROP FOREIGN TABLE Time_Types;
SELECT monetdb_execute('foreign_server', $$DROP TABLE Time_Types$$);

-- test JSON
select monetdb_execute('foreign_server', $$CREATE TABLE json_example (c1 JSON, c2 JSON(512) NOT NULL)$$);
IMPORT FOREIGN SCHEMA "test_u" limit to (json_example) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'json_example';

INSERT INTO json_example values(
'{ "store": {
    "book": [
      { "category": "reference",
        "author": "Nigel Rees",
        "title": "Sayings of the Century",
        "price": 8.95
      },
      { "category": "fiction",
        "author": "Evelyn Waugh",
        "title": "Sword of Honour",
        "price": 12.99
      }
    ],
    "bicycle": {
      "color": "red",
      "price": 19.95
    }
  }
}',

'{ "store": {
    "book": [
      { "category": "reference",
        "author": "Nigel Rees",
        "title": "Sayings of the Century",
        "price": 8.95
      },
      { "category": "fiction",
        "author": "Evelyn Waugh",
        "title": "Sword of Honour",
        "price": 12.99
      }
    ],
    "bicycle": {
      "color": "red",
      "price": 19.95
    }
  }
}');

SELECT * FROM json_example;
SELECT jsonb_path_query(c1::jsonb, '$.store') FROM json_example;  -- use postgresql function

-- CREATE TABLE json_example2(h JSON(512));      -- error

DROP FOREIGN TABLE json_example;
SELECT monetdb_execute('foreign_server', $$DROP TABLE json_example$$);

-- test UUID
select monetdb_execute('foreign_server', $$CREATE TABLE uuid_example(a UUID)$$);
IMPORT FOREIGN SCHEMA "test_u" limit to (uuid_example) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'uuid_example';

INSERT INTO uuid_example VALUES('26d7a80b-7538-4682-a49a-9d0f9676b765');
SELECT * FROM uuid_example;
TRUNCATE uuid_example;
INSERT INTO uuid_example VALUES(gen_random_uuid());
SELECT count(*) FROM uuid_example;
DROP FOREIGN TABLE uuid_example;
SELECT monetdb_execute('foreign_server', $$DROP TABLE uuid_example$$);

-- tes INET
select monetdb_execute('foreign_server', $$CREATE TABLE inet_example(a inet)$$);
IMPORT FOREIGN SCHEMA "test_u" limit to (inet_example) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'inet_example';

INSERT INTO inet_example VALUES('192.168.1.5/24');
SELECT * FROM inet_example;
DROP FOREIGN TABLE inet_example;
SELECT monetdb_execute('foreign_server', $$DROP TABLE inet_example$$);

-- test URL
SELECT monetdb_execute('foreign_server', $$CREATE TABLE URL_example (c1 URL, c2 URL(512) NOT NULL)$$);
IMPORT FOREIGN SCHEMA "test_u" limit to (URL_example) from server foreign_server into public;
-- Verify foreign table is correctly created
SELECT c.relname as "Table",
       s.srvname as "Server",
       array_to_string(ft.ftoptions, ', ') as "FDW options"
FROM pg_foreign_table ft
     JOIN pg_class c ON c.oid = ft.ftrelid
     JOIN pg_foreign_server s ON s.oid = ft.ftserver
WHERE c.relname = 'url_example';

INSERT INTO url_example VALUES('https://github.com/Z-Xiao-M/MonetDB_fdw', 'https://github.com/Z-Xiao-M/MonetDB_fdw');
SELECT * FROM URL_example;
DROP FOREIGN TABLE URL_example;
SELECT monetdb_execute('foreign_server', $$DROP TABLE URL_example$$);

-- CREATE TABLE URL_example(h URL(512));      -- error

-- Switch back to monetdb user to delete test_u user
DROP USER MAPPING FOR CURRENT_USER SERVER foreign_server;
CREATE USER MAPPING FOR CURRENT_USER SERVER foreign_server OPTIONS (user 'monetdb', password 'monetdb');

SELECT monetdb_execute('foreign_server', $$ALTER USER test_u SET SCHEMA sys$$);
SELECT monetdb_execute('foreign_server', $$DROP SCHEMA test_u$$);
SELECT monetdb_execute('foreign_server', $$DROP USER test_u$$);