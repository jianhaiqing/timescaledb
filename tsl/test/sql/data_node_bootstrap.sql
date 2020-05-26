-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- Test functionality of add_data_node() bootstrapping.
-- Most of this already done in other tests, so we check some corner cases.
\c :TEST_DBNAME :ROLE_SUPERUSER;
GRANT USAGE ON FOREIGN DATA WRAPPER timescaledb_fdw TO :ROLE_DEFAULT_PERM_USER;

CREATE OR REPLACE FUNCTION show_data_nodes()
RETURNS TABLE(data_node_name NAME, host TEXT, port INT, dbname NAME)
AS :TSL_MODULE_PATHNAME, 'test_data_node_show' LANGUAGE C;

-- Cleanup from other potential tests that created these databases
SET client_min_messages TO ERROR;
DROP DATABASE IF EXISTS bootstrap_test;
SET client_min_messages TO NOTICE;

SET ROLE :ROLE_DEFAULT_PERM_USER;

-- Super user is required to make remote connection without password
--
-- bootstrap_user     = :ROLE_DEFAULT_PERM_USER
-- bootstrap_database = 'postgres'
\set ON_ERROR_STOP 0
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass');
\set ON_ERROR_STOP 1
SELECT * FROM show_data_nodes();

-- local_user         = :ROLE_SUPERUSER
-- remote_user        = :ROLE_SUPERUSER
-- bootstrap_user     = :ROLE_SUPERUSER
-- bootstrap_database = 'postgres'
RESET ROLE;
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass');
SET ROLE :ROLE_DEFAULT_PERM_USER;
SELECT * FROM show_data_nodes();

-- Ensure database and extensions are installed
\c bootstrap_test :ROLE_SUPERUSER
SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;
\c :TEST_DBNAME :ROLE_SUPERUSER;

-- After delete database and extension should still be there
SELECT * FROM delete_data_node('bootstrap_test', cascade => true);
SELECT * FROM show_data_nodes();
\c bootstrap_test :ROLE_SUPERUSER;
SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;
\c :TEST_DBNAME :ROLE_SUPERUSER;

-- Try to recreate data node with the same name, database and extension exists
--
-- bootstrap_user     = :ROLE_SUPERUSER
-- bootstrap_database = 'postgres'
\set ON_ERROR_STOP 0
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass');
\set ON_ERROR_STOP 1
SELECT * FROM show_data_nodes();

-- Test if_not_exists functionality (no local server, but remote database and extension exists)
--
-- bootstrap_user     = :ROLE_SUPERUSER
-- bootstrap_database = 'postgres'
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass', if_not_exists => true);
SELECT * FROM show_data_nodes();

-- Test if_not_exists functionality (has local server, has database database but no extension installed)
--
-- bootstrap_user     = :ROLE_SUPERUSER
-- bootstrap_database = 'postgres'
\c bootstrap_test :ROLE_SUPERUSER;
SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;

DROP EXTENSION timescaledb CASCADE;

SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;

\c :TEST_DBNAME :ROLE_SUPERUSER;
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass', if_not_exists => true);
\c bootstrap_test :ROLE_SUPERUSER;

SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;

\c :TEST_DBNAME :ROLE_SUPERUSER;

SELECT * FROM delete_data_node('bootstrap_test', cascade => true);
DROP DATABASE bootstrap_test;

-- Test automatic schema creation in a new database. Use template0 to
-- not get extension pre-installed.
CREATE DATABASE bootstrap_schema_test TEMPLATE template0;
\c bootstrap_schema_test :ROLE_SUPERUSER;
CREATE SCHEMA bootstrap_schema;
SET client_min_messages TO error;
CREATE EXTENSION timescaledb WITH SCHEMA bootstrap_schema;

SET client_min_messages TO NOTICE;
SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;

SELECT * FROM bootstrap_schema.add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass');
\c bootstrap_test :ROLE_SUPERUSER;

SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;

\c bootstrap_schema_test :ROLE_SUPERUSER;
SELECT * FROM bootstrap_schema.add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass', if_not_exists => true);
\c :TEST_DBNAME :ROLE_SUPERUSER;
DROP DATABASE bootstrap_schema_test;
DROP DATABASE bootstrap_test;

SET ROLE :ROLE_DEFAULT_CLUSTER_USER;
-- Test with non-superuser
--
-- bootstrap_user     = :ROLE_CLUSTER_SUPERUSER
-- bootstrap_database = 'template1'
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => :'ROLE_DEFAULT_CLUSTER_USER_PASS', bootstrap_user => :'ROLE_CLUSTER_SUPERUSER', bootstrap_password => :'ROLE_CLUSTER_SUPERUSER_PASS', bootstrap_database => 'template1');
\c bootstrap_test :ROLE_DEFAULT_PERM_USER;

SELECT extname, nspname
FROM pg_extension e, pg_namespace n
WHERE e.extnamespace = n.oid;

\c :TEST_DBNAME :ROLE_SUPERUSER;
SELECT * FROM delete_data_node('bootstrap_test', cascade => true);

-- Test for ongoing transaction
BEGIN;
\set ON_ERROR_STOP 0
SELECT * FROM add_data_node('bootstrap_test', database => 'bootstrap_test', password => 'perm_user_pass');
\set ON_ERROR_STOP 1
COMMIT;
SELECT * FROM show_data_nodes();

DROP DATABASE bootstrap_test;

-- Test unusual database names
--
-- bootstrap_user     = :ROLE_SUPERUSER
-- bootstrap_database = 'postgres'
SELECT true FROM add_data_node('bootstrap_test1', database => 'Unusual Name', password => 'perm_user_pass');
SELECT true FROM add_data_node('bootstrap_test1', database => 'Unusual Name', password => 'perm_user_pass', if_not_exists => true);

SELECT true FROM add_data_node('bootstrap_test2', database => U&'\0441\043B\043E\043D', password => 'perm_user_pass');
SELECT true FROM add_data_node('bootstrap_test2', database => U&'\0441\043B\043E\043D', password => 'perm_user_pass', if_not_exists => true);

SELECT count(*) FROM show_data_nodes();
SELECT true FROM pg_database WHERE datname = 'Unusual Name';
SELECT true FROM pg_database WHERE datname = U&'\0441\043B\043E\043D';

SELECT true FROM delete_data_node('bootstrap_test1', cascade => true);
SELECT true FROM delete_data_node('bootstrap_test2', cascade => true);

DROP DATABASE "Unusual Name";
DROP DATABASE U&"\0441\043B\043E\043D";
