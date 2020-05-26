/*
 * Copyright (c) 2018  Timescale, Inc. All Rights Reserved.
 *
 * This file is licensed under the Timescale License,
 * see LICENSE-TIMESCALE at the top of the tsl directory.
 */
#include <postgres.h>
#include <utils/fmgrprotos.h>
#include <utils/builtins.h>
#include <utils/memutils.h>
#include <storage/procarray.h>
#include <foreign/foreign.h>
#include <miscadmin.h>

#include "node_killer.h"
#include "test_utils.h"
#include "guc.h"
#include "export.h"
#include "connection.h"

typedef struct RemoteNodeKiller
{
	pid_t pid;
	PGconn *conn;
} RemoteNodeKiller;

static char *kill_event = NULL;
static RemoteNodeKiller *rnk_event = NULL;

TS_FUNCTION_INFO_V1(ts_remote_node_killer_set_event);

RemoteNodeKiller *
remote_node_killer_init(PGconn *conn)
{
	PGresult *res;
	char *sql = "SELECT pg_backend_pid()";
	char *pid_string;
	unsigned long pid_long;

	rnk_event = palloc(sizeof(RemoteNodeKiller));

	rnk_event->conn = conn;

	res = PQexec(conn, sql);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		/* do not throw error here to avoid recursive abort */
		/* remote_connection_report_error(ERROR, res, conn, false, sql);  */
		rnk_event->pid = 0;
		return rnk_event;
	}

	TestAssertTrue(1 == PQntuples(res));
	TestAssertTrue(1 == PQnfields(res));

	pid_string = PQgetvalue(res, 0, 0);
	pid_long = strtol(pid_string, NULL, 10);

	rnk_event->pid = (pid_t) pid_long;

	PQclear(res);
	return rnk_event;
}

void
remote_node_killer_kill(RemoteNodeKiller *rnk)
{
	/*
	 * do not use pg_terminate_backend here because that does permission
	 * checks through the catalog which requires you to be in a transaction
	 */
	PGPROC *proc = BackendPidGetProc(rnk->pid);

	if (proc == NULL)
		ereport(WARNING, (errmsg("PID %d is not a PostgreSQL server process", rnk->pid)));
	kill_event = NULL;
#ifdef HAVE_SETSID
	if (kill(-rnk->pid, SIGTERM))
#else
	if (kill(rnk->pid, SIGTERM))
#endif
		ereport(WARNING, (errmsg("could not send signal to process %d: %m", rnk->pid)));
}
