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
\d+ Numeric_Types

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
\d+ bool_Types

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
SELECT monetdb_execute('foreign_server', $$ALTER USER test_u SET SCHEMA sys$$);
SELECT monetdb_execute('foreign_server', $$DROP SCHEMA test_u$$);
SELECT monetdb_execute('foreign_server', $$DROP USER test_u$$);