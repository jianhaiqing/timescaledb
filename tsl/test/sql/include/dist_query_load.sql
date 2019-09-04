-- This file and its contents are licensed under the Timescale License.
-- Please see the included NOTICE for copyright information and
-- LICENSE-TIMESCALE for a copy of the license.

-- Cleanup from other tests that might have created these databases
SET client_min_messages TO ERROR;
SET ROLE :ROLE_CLUSTER_SUPERUSER;
DROP DATABASE IF EXISTS data_node_1;
DROP DATABASE IF EXISTS data_node_2;
DROP DATABASE IF EXISTS data_node_3;

SELECT * FROM add_data_node('data_node_1', host => 'localhost',
                            database => 'data_node_1');
SELECT * FROM add_data_node('data_node_2', host => 'localhost',
                            database => 'data_node_2');
SELECT * FROM add_data_node('data_node_3', host => 'localhost',
                            database => 'data_node_3');
GRANT USAGE ON FOREIGN SERVER data_node_1, data_node_2, data_node_3 TO :ROLE_1;

SET ROLE :ROLE_1;
CREATE TABLE hyper (time TIMESTAMPTZ, device INT, temp FLOAT);
CREATE TABLE hyper_repart (LIKE hyper);
SELECT create_distributed_hypertable('hyper', 'time', 'device', 3,
                                     chunk_time_interval => interval '18 hours');
SELECT create_distributed_hypertable('hyper_repart', 'time', 'device', 3,
                                     chunk_time_interval => interval '18 hours');

SELECT setseed(1);
INSERT INTO hyper
SELECT t, (abs(timestamp_hash(t::timestamp)) % 10) + 1, random() * 80
FROM generate_series('2019-01-01'::timestamptz, '2019-01-04'::timestamptz, '1 minute') as t;

-- Repartition the data set on one table so that we can compare
-- queries on repartitioned and non-repartitioned tables
INSERT INTO hyper_repart
SELECT * FROM hyper
WHERE time < '2019-01-02 05:10'::timestamptz;
SELECT * FROM set_number_partitions('hyper_repart', 2);
INSERT INTO hyper_repart
SELECT * FROM hyper
WHERE time >= '2019-01-02 05:10'::timestamptz
AND time < '2019-01-03 01:22'::timestamptz;
SELECT * FROM set_number_partitions('hyper_repart', 5);
INSERT INTO hyper_repart
SELECT * FROM hyper
WHERE time >= '2019-01-03 01:22'::timestamptz;

SELECT d.hypertable_id, d.id, ds.range_start, ds.range_end
FROM _timescaledb_catalog.dimension d, _timescaledb_catalog.dimension_slice ds
WHERE num_slices IS NOT NULL
AND d.id = ds.dimension_id
ORDER BY 1, 2, 3;
