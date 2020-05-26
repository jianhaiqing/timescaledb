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
	rnk_event = palloc(sizeof(RemoteNodeKiller));
	rnk_event->conn = conn;
	rnk_event->pid = remote_connecton_get_remote_pid(conn);

	/* do not throw error here on pid = 0 to avoid recursive abort */
	/* remote_connection_report_error(ERROR, res, conn, false, sql);  */
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
