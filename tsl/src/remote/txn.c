/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <access/xact.h>
#include <utils/builtins.h>
#include <utils/snapmgr.h>

#include "compat.h"
#if PG12_LT
#include <utils/tqual.h>
#endif

#include "txn.h"
#include "connection.h"
#include "scanner.h"
#include "catalog.h"
#include "txn_id.h"

/* This seemingly long timeout matches what postgres_fdw uses. */
#define DEFAULT_EXEC_CLEANUP_TIMEOUT_MS 30000

/*
 * This RemoteTxn represents one remote end in a distributed txn.
 * Thus, a distributed txn is made up of a collection remote txn.
 * Each remote txn corresponds to one remote connection and there
 * is a unique remote connection per TSConnectionId used in the
 * distributed txn. Because of this uniqueness property,
 * the connection id appears first in the object, to allow
 * it to be a hash key.
 *
 * The "conn" pointer can be NULL if we don't currently have a live connection.
 * When we do have a connection, xact_depth tracks the current depth of
 * transactions and subtransactions open on the remote side.  We need to issue
 * commands at the same nesting depth on the remote as we're executing at
 * ourselves, so that rolling back a subtransaction will kill the right
 * queries and not the wrong ones.
 */

typedef struct RemoteTxn
{
	TSConnectionId id;  /* hash key (must be first) */
	TSConnection *conn; /* connection to data node, or NULL */
	/* Remaining fields are invalid when conn is NULL: */
	int xact_depth;			/* 0 = no xact open, 1 = main xact open, 2 =
							 * one level of subxact open, etc */
	bool have_prep_stmt;	/* have we prepared any stmts in this xact? */
	bool have_subtxn_error; /* have any subxacts aborted in this xact? */
	RemoteTxnId *remote_txn_id;
} RemoteTxn;

/*
 * Start remote transaction or subtransaction, if it hasn't been
 * already started (e.g. by a previous command in the same txn).
 *
 * We always use at least REPEATABLE READ in the remote session.
 * This is important even for cases where we use the a single connection to
 * a data node. This is because a single frontend command may cause multiple
 * remote commands to be executed (e.g. a join of two tables on one remote
 * node might not be pushed down and instead two different queries are sent
 * to the remote node, one for each table in the join). Since in READ
 * COMMITED the snapshot is refreshed on each command, the semantics are off
 * when multiple commands are meant to be part of the same one.
 *
 * This isn't great but we have no alternative unless we ensure that each frontend
 * command always translates to one backend query or if we had some other way to
 * control which remote queries share a snapshot or when a snapshot is refreshed.
 *
 * NOTE: this does not guarantee any kind of snapshot isolation to different connections
 * to the same data node. That only happens if we use multiple connection ids to the same data node
 * in one access node transaction. Thus, such connections that use different users will potentially
 * see inconsistent results. To solve this problem of inconsistent results, we could export the
 * snapshot of the first connection to a remote node using pg_export_snapshot() and then use that
 * using SET TRANSACTION SNAPSHOT xxxx across all other connections to that node during the
 * transaction. However, given that we currently don't have snapshot isolation across different
 * nodes, we don't want to commit to the overhead of exporting snapshots at this time.
 */
void
remote_txn_begin(RemoteTxn *entry, int curlevel)
{
	/* Start main transaction if we haven't yet */
	if (entry->xact_depth == 0)
	{
		const char *sql;

		elog(DEBUG3, "starting remote transaction on connection %p", entry->conn);

		if (IsolationIsSerializable())
			sql = "START TRANSACTION ISOLATION LEVEL SERIALIZABLE";
		else
			sql = "START TRANSACTION ISOLATION LEVEL REPEATABLE READ";
		remote_connection_cmd_ok(entry->conn, sql);
		entry->xact_depth = 1;
	}

	/*
	 * If we're in a subtransaction, stack up savepoints to match our level.
	 * This ensures we can rollback just the desired effects when a
	 * subtransaction aborts.
	 */
	while (entry->xact_depth < curlevel)
	{
		remote_connection_cmdf_ok(entry->conn, "SAVEPOINT s%d", entry->xact_depth + 1);
		entry->xact_depth++;
	}
}

bool
remote_txn_is_still_in_progress(TransactionId frontend_xid)
{
	if (TransactionIdIsCurrentTransactionId(frontend_xid))
		elog(ERROR, "checking if a commit is still in progress on same txn");

	return XidInMVCCSnapshot(frontend_xid, GetTransactionSnapshot());
}

size_t
remote_txn_size()
{
	return sizeof(RemoteTxn);
}

void
remote_txn_init(RemoteTxn *entry, TSConnection *conn)
{
	ForeignServer *server = GetForeignServer(entry->id.server_id);

	Assert(NULL != conn);

	/* Reset all transient state fields, to be sure all are clean */
	entry->xact_depth = 0;
	entry->have_prep_stmt = false;
	entry->have_subtxn_error = false;
	entry->remote_txn_id = NULL;

	/* Now try to make the connection */
	/* in connection  */
	entry->conn = conn;

	elog(DEBUG3,
		 "new connection %p for data node \"%s\" (server "
		 "oid %u, userid %u)",
		 entry->conn,
		 server->servername,
		 entry->id.server_id,
		 entry->id.user_id);
}

void
remote_txn_set_will_prep_statement(RemoteTxn *entry, RemoteTxnPrepStmtOption prep_stmt_option)
{
	bool will_prep_stmt = (prep_stmt_option == REMOTE_TXN_USE_PREP_STMT);

	entry->have_prep_stmt |= will_prep_stmt;
}

TSConnection *
remote_txn_get_connection(RemoteTxn *txn)
{
	return txn->conn;
}

TSConnectionId
remote_txn_get_connection_id(RemoteTxn *txn)
{
	return txn->id;
}

void
remote_txn_report_prepare_transaction_result(RemoteTxn *txn, bool success)
{
	if (!success)
		txn->remote_txn_id = NULL;
}

/*
 * This function submits commands to remote nodes during (sub)abort processing.
 * Because remote nodes can be in a weird state and at the same time errors should
 * not be thrown here, the processing here is a bit different.
 *
 * We submit a query during and wait up to 30 seconds for the result. All errors
 * are reported as WARNINGS into the log.
 *
 * If the query is executed without error, the return value is true.
 * If the query can't be sent, errors out, or times out, the return value is false.
 */
static bool
exec_cleanup_command(TSConnection *conn, const char *query)
{
	TimestampTz end_time;
	AsyncRequest *req;
	AsyncRequestSet *set = async_request_set_create();
	AsyncResponse *response;
	AsyncResponseResult *result;
	PGresult *pg_result;
	bool success = false;

	/*
	 * If it takes too long to execute a cleanup query, assume the connection
	 * is dead.  It's fairly likely that this is why we aborted in the first
	 * place (e.g. statement timeout, user cancel), so the timeout shouldn't
	 * be too long.
	 */
	end_time = TimestampTzPlusMilliseconds(GetCurrentTimestamp(), DEFAULT_EXEC_CLEANUP_TIMEOUT_MS);

	/*
	 * Submit a query.  Since we don't use non-blocking mode, this also can
	 * block.  But its risk is relatively small, so we ignore that for now.
	 */
	req = async_request_send_with_error(conn, query, WARNING);
	if (req == NULL)
		return false;

	async_request_set_add(set, req);

	response = async_request_set_wait_any_response_deadline(set, WARNING, end_time);
	Assert(response != NULL);

	switch (async_response_get_type(response))
	{
		case RESPONSE_TIMEOUT:
			elog(DEBUG3, "abort processing: timeout executing %s", query);
			success = false;
			break;
		case RESPONSE_COMMUNICATION_ERROR:
			elog(DEBUG3, "abort processing: communication error executing %s", query);
			success = false;
			break;
		case RESPONSE_RESULT:
			result = (AsyncResponseResult *) response;
			pg_result = async_response_result_get_pg_result(result);
			if (PQresultStatus(pg_result) != PGRES_COMMAND_OK)
			{
				elog(DEBUG3, "abort processing: error in result executing %s", query);
				success = false;
			}
			else
				success = true;
			break;
	}

	if (success)
	{
		async_response_close(response);

		/* that should have been the last response from the set */
		response = async_request_set_wait_any_response_deadline(set, WARNING, end_time);
		Assert(response == NULL);
	}
	else
	{
		async_response_report_error(response, WARNING);

		/* drain the set until empty of all possibly queued errors */
		while ((response = async_request_set_wait_any_response_deadline(set, WARNING, end_time)))
			;
	}
	return success;
}

#if DEBUG
/* Prepared statements can leak if the were created during a subtxn
 * and the subtxn rolled back before the prepared stmt was deallocated.
 * This function checks for such leaks inside of tests (thus only compiled
 * in DEBUG mode). It can be quite expensive so not run under normal operations.
 */
void
remote_txn_check_for_leaked_prepared_statements(RemoteTxn *entry)
{
	PGresult *res;
	char *count_string;

	if (PQTRANS_IDLE != PQtransactionStatus(remote_connection_get_pg_conn(entry->conn)))
		return;

	res = remote_connection_exec(entry->conn, "SELECT count(*) FROM pg_prepared_statements");

	Assert(1 == PQntuples(res));
	Assert(1 == PQnfields(res));

	count_string = PQgetvalue(res, 0, 0);
	if (strcmp("0", count_string) != 0)
		elog(WARNING, "connection leaked prepared statement");

	remote_result_close(res);
}
#endif

bool
remote_txn_abort(RemoteTxn *entry)
{
	const char *abort_sql;

	if (entry->remote_txn_id == NULL)
		abort_sql = "ABORT TRANSACTION";
	else
		abort_sql = remote_txn_id_rollback_prepared_sql(entry->remote_txn_id);

	entry->remote_txn_id = NULL;

	Assert(entry->conn != NULL);
	Assert(entry->xact_depth > 0);

	elog(DEBUG3, "aborting remote transaction on connection %p", entry->conn);

	/*
	 * Don't try to recover the connection if we're already in error recursion
	 * trouble. This is a really bad case and so controlled cleanup cannot
	 * happen here. The calling function will instead break this ongoing
	 * connection and so no cleanup is necessary.
	 */
	if (in_error_recursion_trouble())
		return false;

	switch (PQtransactionStatus(remote_connection_get_pg_conn(entry->conn)))
	{
		case PQTRANS_IDLE:
		case PQTRANS_INTRANS:
		case PQTRANS_INERROR:
			/* ready for more commands */
			break;
		case PQTRANS_ACTIVE:

			/*
			 * We are here if a command has been submitted to the data node
			 * by using an asynchronous execution function and the
			 * command had not yet completed.  If so, request cancellation of
			 * the command.
			 */
			if (!remote_connection_cancel_query(entry->conn))
				return false;
			break;
		case PQTRANS_UNKNOWN:
			return false;
	}

	/* At this point any on going queries should have completed */
	remote_connection_set_processing(entry->conn, false);

	if (!exec_cleanup_command(entry->conn, abort_sql))
		return false;

	/*
	 * Assume we might may have not deallocated all the prepared statements we
	 * created because the deallocation would have happened after the abort.
	 *
	 * prepared stmts are per session not per transaction. But we don't want
	 * prepared_stmts to survive transactions in our use case.
	 */
	if (entry->have_prep_stmt && !exec_cleanup_command(entry->conn, "DEALLOCATE ALL"))
		return false;

	entry->have_prep_stmt = false;
	entry->have_subtxn_error = false;

	return true;
}

/*
 * If there were any errors in subtransactions, and we made prepared
 * statements, those prepared statements may not have been cleared
 * because of the subtxn error. Thus, do a DEALLOCATE ALL to make sure
 * we get rid of all prepared statements.
 *
 * This is annoying and not terribly bulletproof, but it's
 * probably not worth trying harder.
 */
void
remote_txn_deallocate_prepared_stmts_if_needed(RemoteTxn *entry)
{
	Assert(entry->conn != NULL && entry->xact_depth > 0);

	if (entry->have_prep_stmt && entry->have_subtxn_error)
	{
		AsyncRequestSet *set = async_request_set_create();
		AsyncResponse *response;
		AsyncResponseResult *result;

		async_request_set_add(set, async_request_send(entry->conn, "DEALLOCATE ALL"));

		response = async_request_set_wait_any_response(set, WARNING);

		switch (async_response_get_type(response))
		{
			case RESPONSE_RESULT:
				result = (AsyncResponseResult *) response;
				if (PQresultStatus(async_response_result_get_pg_result(result)) == PGRES_COMMAND_OK)
				{
					async_response_close(response);
					break;
				}
				/* fallthrough */
			default:
				async_response_report_error(response, WARNING);
		}

		response = async_request_set_wait_any_response(set, WARNING);
		Assert(response == NULL);
	}
	entry->have_prep_stmt = false;
	entry->have_subtxn_error = false;
}

AsyncRequest *
remote_txn_async_send_commit(RemoteTxn *entry)
{
	Assert(entry->conn != NULL);
	Assert(entry->xact_depth > 0);

	elog(DEBUG3, "committing remote transaction on connection %p", entry->conn);
	return async_request_send(entry->conn, "COMMIT TRANSACTION");
}

void
remote_txn_write_persistent_record(RemoteTxn *entry)
{
	entry->remote_txn_id = remote_txn_persistent_record_write(entry->id);
}

AsyncRequest *
remote_txn_async_send_prepare_transaction(RemoteTxn *entry)
{
	Assert(entry->conn != NULL);
	Assert(entry->xact_depth > 0);
	Assert(entry->remote_txn_id != NULL);

	elog(DEBUG3,
		 "2pc: preparing remote transaction on connection %p: %s",
		 entry->conn,
		 remote_txn_id_out(entry->remote_txn_id));
	return async_request_send(entry->conn,
							  remote_txn_id_prepare_transaction_sql(entry->remote_txn_id));
}

AsyncRequest *
remote_txn_async_send_commit_prepared(RemoteTxn *entry)
{
	Assert(entry->conn != NULL);
	Assert(entry->remote_txn_id != NULL);

	elog(DEBUG3,
		 "2pc: commiting remote transaction on connection %p: '%s'",
		 entry->conn,
		 remote_txn_id_out(entry->remote_txn_id));
	return async_request_send_with_error(entry->conn,
										 remote_txn_id_commit_prepared_sql(entry->remote_txn_id),
										 WARNING);
}

bool
remote_txn_sub_txn_abort(RemoteTxn *entry, int curlevel)
{
	StringInfoData sql;
	PGconn *pg_conn = remote_connection_get_pg_conn(entry->conn);

	Assert(entry->xact_depth == curlevel);
	Assert(entry->xact_depth > 1);
	initStringInfo(&sql);

	if (in_error_recursion_trouble())
		return false;

	if (PQtransactionStatus(pg_conn) != PQTRANS_INTRANS &&
		PQtransactionStatus(pg_conn) != PQTRANS_INERROR)
		return false;

	entry->have_subtxn_error = true;

	/*
	 * If a command has been submitted to the data node by using an
	 * asynchronous execution function, the command might not have yet
	 * completed.  Check to see if a command is still being processed by the
	 * data node, and if so, request cancellation of the command.
	 */
	if (PQtransactionStatus(pg_conn) == PQTRANS_ACTIVE &&
		!remote_connection_cancel_query(entry->conn))
		return false;

	/* Rollback all remote subtransactions during abort */
	appendStringInfo(&sql, "ROLLBACK TO SAVEPOINT s%d", entry->xact_depth);
	if (!exec_cleanup_command(entry->conn, sql.data))
		return false;

	resetStringInfo(&sql);
	appendStringInfo(&sql, "RELEASE SAVEPOINT s%d", entry->xact_depth);
	if (!exec_cleanup_command(entry->conn, sql.data))
		return false;

	Assert(entry->xact_depth > 0);
	entry->xact_depth--;
	return true;
}

bool
remote_txn_is_at_sub_txn_level(RemoteTxn *entry, int curlevel)
{
	/*
	 * We only care about connections with open remote subtransactions of the
	 * current level.
	 */
	Assert(entry->conn != NULL);
	if (entry->xact_depth < curlevel)
		return false;

	if (entry->xact_depth > curlevel)
		elog(ERROR, "missed cleaning up remote subtransaction at level %d", entry->xact_depth);

	Assert(entry->xact_depth == curlevel);

	return true;
}

void
remote_txn_sub_txn_pre_commit(RemoteTxn *entry, int curlevel)
{
	Assert(entry->xact_depth == curlevel);
	remote_connection_cmdf_ok(entry->conn, "RELEASE SAVEPOINT s%d", curlevel);
	Assert(entry->xact_depth > 0);
	entry->xact_depth--;
}

/*
 *		Persistent Record stuff
 */

static int
persistent_record_pkey_scan(const RemoteTxnId *id, tuple_found_func tuple_found, LOCKMODE lock_mode)
{
	Catalog *catalog = ts_catalog_get();
	ScanKeyData scankey[1];
	ScannerCtx scanctx = {
		.table = catalog->tables[REMOTE_TXN].id,
		.index = catalog_get_index(catalog, REMOTE_TXN, REMOTE_TXN_PKEY_IDX),
		.nkeys = 1,
		.scankey = scankey,
		.tuple_found = tuple_found,
		.lockmode = lock_mode,
		.limit = 1,
		.scandirection = ForwardScanDirection,
	};

	ScanKeyInit(&scankey[0],
				Anum_remote_txn_pkey_idx_remote_transaction_id,
				BTEqualStrategyNumber,
				F_TEXTEQ,
				CStringGetTextDatum(remote_txn_id_out(id)));

	return ts_scanner_scan(&scanctx);
}

bool
remote_txn_persistent_record_exists(const RemoteTxnId *parsed)
{
	return persistent_record_pkey_scan(parsed, NULL, AccessShareLock) > 0;
}

static ScanTupleResult
persistent_record_tuple_delete(TupleInfo *ti, void *data)
{
	ts_catalog_delete(ti->scanrel, ti->tuple);
	return SCAN_CONTINUE;
}

int
remote_txn_persistent_record_delete_for_data_node(Oid foreign_server_oid)
{
	Catalog *catalog = ts_catalog_get();
	ScanKeyData scankey[1];
	ScannerCtx scanctx;
	ForeignServer *server = GetForeignServer(foreign_server_oid);

	ScanKeyInit(&scankey[0],
				Anum_remote_txn_data_node_name_idx_data_node_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				DirectFunctionCall1(namein, CStringGetDatum(server->servername)));

	scanctx = (ScannerCtx){
		.table = catalog->tables[REMOTE_TXN].id,
		.index = catalog_get_index(catalog, REMOTE_TXN, REMOTE_TXN_DATA_NODE_NAME_IDX),
		.nkeys = 1,
		.scankey = scankey,
		.tuple_found = persistent_record_tuple_delete,
		.lockmode = RowExclusiveLock,
		.scandirection = ForwardScanDirection,
	};

	return ts_scanner_scan(&scanctx);
}

static void
persistent_record_insert_relation(Relation rel, RemoteTxnId *id)
{
	TupleDesc desc = RelationGetDescr(rel);
	Datum values[Natts_remote_txn];
	bool nulls[Natts_remote_txn] = { false };
	CatalogSecurityContext sec_ctx;
	ForeignServer *server = GetForeignServer(id->id.server_id);

	values[AttrNumberGetAttrOffset(Anum_remote_txn_data_node_name)] =
		DirectFunctionCall1(namein, CStringGetDatum(server->servername));
	values[AttrNumberGetAttrOffset(Anum_remote_txn_remote_transaction_id)] =
		CStringGetTextDatum(remote_txn_id_out(id));

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_insert_values(rel, desc, values, nulls);
	ts_catalog_restore_user(&sec_ctx);
}

/*
 * Add a commit record to catalog.
 */
RemoteTxnId *
remote_txn_persistent_record_write(TSConnectionId cid)
{
	RemoteTxnId *id = remote_txn_id_create(GetTopTransactionId(), cid);
	Catalog *catalog = ts_catalog_get();
	Relation rel;

	rel = table_open(catalog->tables[REMOTE_TXN].id, RowExclusiveLock);
	persistent_record_insert_relation(rel, id);
	table_close(rel, RowExclusiveLock);

	return id;
}
