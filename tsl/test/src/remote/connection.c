/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <utils/fmgrprotos.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <storage/procarray.h>
#include <foreign/foreign.h>
#include <foreign/fdwapi.h>
#include <miscadmin.h>
#include <access/reloptions.h>
#include <catalog/pg_user_mapping.h>
#include <catalog/pg_foreign_server.h>
#include <commands/dbcommands.h>
#include <nodes/makefuncs.h>
#include <nodes/pg_list.h>
#include <utils/guc.h>

#include "connection.h"
#include "export.h"
#include "test_utils.h"
#include "connection.h"

PGconn *
get_connection(void)
{
	return remote_connection_open("testdb",
								  list_make3(makeDefElem("user",
														 (Node *) makeString(
															 GetUserNameFromId(GetUserId(), false)),
														 -1),
											 makeDefElem("dbname",
														 (Node *) makeString(
															 get_database_name(MyDatabaseId)),
														 -1),
											 makeDefElem("port",
														 (Node *) makeString(
															 pstrdup(GetConfigOption("port",
																					 false,
																					 false))),
														 -1)),
								  NIL);
}

TS_FUNCTION_INFO_V1(tsl_test_remote_connection);

static void
test_options()
{
	TestAssertTrue(remote_connection_valid_user_option("user"));
	TestAssertTrue(!remote_connection_valid_user_option("port"));
	TestAssertTrue(!remote_connection_valid_user_option("xxx"));
	TestAssertTrue(!remote_connection_valid_user_option("fallback_application_name"));

	TestAssertTrue(remote_connection_valid_server_option("port"));
	TestAssertTrue(!remote_connection_valid_server_option("user"));
	TestAssertTrue(!remote_connection_valid_server_option("xxx"));
	TestAssertTrue(!remote_connection_valid_server_option("fallback_application_name"));
}

static void
test_numbers_associated_with_connections()
{
	PGconn *conn = get_connection();
	TestAssertTrue(remote_connection_get_cursor_number() == 1);
	TestAssertTrue(remote_connection_get_cursor_number() == 2);
	TestAssertTrue(remote_connection_get_cursor_number() == 3);
	remote_connection_reset_cursor_number();
	TestAssertTrue(remote_connection_get_cursor_number() == 1);
	TestAssertTrue(remote_connection_get_cursor_number() == 2);

	TestAssertTrue(remote_connection_get_prep_stmt_number() == 1);
	TestAssertTrue(remote_connection_get_prep_stmt_number() == 2);
	TestAssertTrue(remote_connection_get_prep_stmt_number() == 3);
	remote_connection_close(conn);
}

static void
test_simple_queries()
{
	PGconn *conn = get_connection();
	PGresult *res;
	remote_connection_query_ok_result(conn, "SELECT 1");
	remote_connection_query_ok_result(conn, "SET search_path = pg_catalog");

	res = remote_connection_query_any_result(conn, "SELECT 1");
	TestAssertTrue(PQresultStatus(res) == PGRES_TUPLES_OK);
	remote_connection_result_close(res);
	res = remote_connection_query_any_result(conn, "SELECT abc");
	TestAssertTrue(PQresultStatus(res) != PGRES_TUPLES_OK);
	remote_connection_result_close(res);
	res = remote_connection_query_any_result(conn, "SET search_path = pg_catalog");
	TestAssertTrue(PQresultStatus(res) == PGRES_COMMAND_OK);
	remote_connection_result_close(res);
	res = remote_connection_query_any_result(conn, "SET 123 = 123");
	TestAssertTrue(PQresultStatus(res) != PGRES_COMMAND_OK);
	remote_connection_result_close(res);

	remote_connection_exec_ok_command(conn, "SET search_path = pg_catalog");
	/* not a command */
	TestEnsureError(remote_connection_exec_ok_command(conn, "SELECT 1"));
	remote_connection_close(conn);
}

static void
test_prepared_stmts()
{
	PGconn *conn = get_connection();
	const char **params = (const char **) palloc(sizeof(char *) * 5);
	PreparedStmt *prep;
	PGresult *res;

	prep = remote_connection_prepare(conn, "SELECT 3", 0);
	res = remote_connection_query_prepared_ok_result(prep, NULL);
	TestAssertTrue(PQresultStatus(res) == PGRES_TUPLES_OK);
	TestAssertTrue(strcmp(PQgetvalue(res, 0, 0), "3") == 0);
	remote_connection_result_close(res);
	prepared_stmt_close(prep);

	prep = remote_connection_prepare(conn, "SELECT $1, $3, $2", 3);
	params[0] = "2";
	params[1] = "4";
	params[2] = "8";
	res = remote_connection_query_prepared_ok_result(prep, params);
	TestAssertTrue(PQresultStatus(res) == PGRES_TUPLES_OK);
	TestAssertTrue(strcmp(PQgetvalue(res, 0, 0), "2") == 0);
	TestAssertTrue(strcmp(PQgetvalue(res, 0, 1), "8") == 0);
	TestAssertTrue(strcmp(PQgetvalue(res, 0, 2), "4") == 0);
	remote_connection_result_close(res);
	prepared_stmt_close(prep);

	/* malformed sql (missing commas) */
	TestEnsureError(prep = remote_connection_prepare(conn, "SELECT $1 $3 $2", 3));

	remote_connection_close(conn);
}

static void
test_params()
{
	PGconn *conn = get_connection();
	const char **params = (const char **) palloc(sizeof(char *) * 5);
	PGresult *res;

	params[0] = "2";

	res = remote_connection_query_with_params_ok_result(conn, "SELECT $1", 1, params);
	TestAssertTrue(PQresultStatus(res) == PGRES_TUPLES_OK);
	TestAssertTrue(strcmp(PQgetvalue(res, 0, 0), "2") == 0);
	remote_connection_result_close(res);

	TestEnsureError(
		res = remote_connection_query_with_params_ok_result(conn, "SELECT 1 2 3", 1, params));

	remote_connection_close(conn);
}

Datum
tsl_test_remote_connection(PG_FUNCTION_ARGS)
{
	test_options();
	test_numbers_associated_with_connections();
	test_simple_queries();
	test_prepared_stmts();
	test_params();

	PG_RETURN_VOID();
}

pid_t
remote_connecton_get_remote_pid(PGconn *conn)
{
	PGresult *res;
	char *sql = "SELECT pg_backend_pid()";
	char *pid_string;
	unsigned long pid_long;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		return 0;

	TestAssertTrue(1 == PQntuples(res));
	TestAssertTrue(1 == PQnfields(res));

	pid_string = PQgetvalue(res, 0, 0);
	pid_long = strtol(pid_string, NULL, 10);

	PQclear(res);
	return pid_long;
}

char *
remote_connecton_get_application_name(PGconn *conn)
{
	PGresult *res;
	char *sql = "SELECT application_name from  pg_stat_activity where pid = pg_backend_pid()";
	char *app_name;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		return 0;

	TestAssertTrue(1 == PQntuples(res));
	TestAssertTrue(1 == PQnfields(res));

	app_name = pstrdup(PQgetvalue(res, 0, 0));

	PQclear(res);
	return app_name;
}
