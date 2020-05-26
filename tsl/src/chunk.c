/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <foreign/foreign.h>
#include <catalog/pg_foreign_server.h>
#include <catalog/pg_foreign_table.h>
#include <catalog/dependency.h>
#include <access/htup_details.h>
#include <access/xact.h>
#include <utils/syscache.h>
#include <utils/inval.h>
#include <miscadmin.h>

#include <chunk_data_node.h>
#include <errors.h>

#include "chunk.h"
#include "data_node.h"
#include "deparse.h"
#include "remote/dist_commands.h"

static bool
chunk_set_foreign_server(Chunk *chunk, ForeignServer *new_server)
{
	Relation ftrel;
	HeapTuple tuple;
	HeapTuple copy;
	Datum values[Natts_pg_foreign_table];
	bool nulls[Natts_pg_foreign_table];
	CatalogSecurityContext sec_ctx;
	Oid old_server_id;
	long updated;
	ListCell *lc;
	bool new_server_found = false;

	foreach (lc, chunk->data_nodes)
	{
		ChunkDataNode *cdn = lfirst(lc);

		if (cdn->foreign_server_oid == new_server->serverid)
		{
			new_server_found = true;
			break;
		}
	}

	if (!new_server_found)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("chunk \"%s\" does not exist on server \"%s\"",
						get_rel_name(chunk->table_id),
						new_server->servername)));

	tuple = SearchSysCache1(FOREIGNTABLEREL, ObjectIdGetDatum(chunk->table_id));

	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("chunk \"%s\" is not a foreign table", get_rel_name(chunk->table_id))));

	ftrel = table_open(ForeignTableRelationId, RowExclusiveLock);

	heap_deform_tuple(tuple, RelationGetDescr(ftrel), values, nulls);

	old_server_id =
		DatumGetObjectId(values[AttrNumberGetAttrOffset(Anum_pg_foreign_table_ftserver)]);

	if (old_server_id == new_server->serverid)
	{
		table_close(ftrel, RowExclusiveLock);
		ReleaseSysCache(tuple);
		return false;
	}

	values[AttrNumberGetAttrOffset(Anum_pg_foreign_table_ftserver)] =
		ObjectIdGetDatum(new_server->serverid);

	copy = heap_form_tuple(RelationGetDescr(ftrel), values, nulls);

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_update_tid(ftrel, &tuple->t_self, copy);
	ts_catalog_restore_user(&sec_ctx);

	table_close(ftrel, RowExclusiveLock);
	heap_freetuple(copy);
	ReleaseSysCache(tuple);
	/* invalidate foreign table cache */
	CacheInvalidateRelcacheByRelid(ForeignTableRelationId);
	/* update dependencies between foreign table and foreign server */
	updated = changeDependencyFor(RelationRelationId,
								  chunk->table_id,
								  ForeignServerRelationId,
								  old_server_id,
								  new_server->serverid);
	if (updated != 1)
		elog(ERROR, "could not update data node for chunk \"%s\"", get_rel_name(chunk->table_id));

	/* make changes visible */
	CommandCounterIncrement();

	return true;
}

void
chunk_update_foreign_server_if_needed(int32 chunk_id, Oid existing_server_id)
{
	ListCell *lc;
	ChunkDataNode *new_server = NULL;
	Chunk *chunk = ts_chunk_get_by_id(chunk_id, true);
	ForeignTable *foreign_table = NULL;

	Assert(chunk->relkind == RELKIND_FOREIGN_TABLE);
	foreign_table = GetForeignTable(chunk->table_id);

	/* no need to update since foreign table doesn't reference server we try to remove */
	if (existing_server_id != foreign_table->serverid)
		return;

	Assert(list_length(chunk->data_nodes) > 1);

	foreach (lc, chunk->data_nodes)
	{
		new_server = lfirst(lc);
		if (new_server->foreign_server_oid != existing_server_id)
			break;
	}
	Assert(new_server != NULL);

	chunk_set_foreign_server(chunk, GetForeignServer(new_server->foreign_server_oid));
}

Datum
chunk_set_default_data_node(PG_FUNCTION_ARGS)
{
	Oid chunk_relid = PG_ARGISNULL(0) ? InvalidOid : PG_GETARG_OID(0);
	const char *node_name = PG_ARGISNULL(1) ? NULL : PG_GETARG_CSTRING(1);
	ForeignServer *server;
	Chunk *chunk;

	if (!OidIsValid(chunk_relid))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid chunk: cannot be NULL")));

	chunk = ts_chunk_get_by_relid(chunk_relid, false);

	if (NULL == chunk)
		ereport(ERROR,
				(errcode(ERRCODE_TS_CHUNK_NOT_EXIST),
				 errmsg("relation \"%s\" is not a chunk", get_rel_name(chunk_relid))));

	ts_hypertable_permissions_check(chunk->hypertable_relid, GetUserId());

	server = data_node_get_foreign_server(node_name, ACL_USAGE, false);

	Assert(NULL != server);

	PG_RETURN_BOOL(chunk_set_foreign_server(chunk, server));
}

void
chunk_drop_remote_chunks(Name table_name, Name schema_name, Datum older_than_datum,
						 Datum newer_than_datum, Oid older_than_type, Oid newer_than_type,
						 bool cascade, bool cascades_to_materializations, bool verbose,
						 List *data_node_oids)
{
	List *data_node_names;
	const char *sql_cmd;

	if (table_name == NULL && schema_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot use wildcard to drop chunks on distributed hypertables"),
				 errhint("Drop chunks on each distributed hypertable individually.")));

	/* The schema name must be present when dropping remote chunks because the
	 * search path on the connection is always set to pg_catalog. Thus, the
	 * data node will not be able to resolve the same hypertables without the
	 * schema. */
	if (schema_name == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("schema required when dropping chunks on distributed hypertables")));

	data_node_names = data_node_oids_to_node_name_list(data_node_oids, ACL_USAGE);
	sql_cmd = deparse_drop_chunks_func(table_name,
									   schema_name,
									   older_than_datum,
									   newer_than_datum,
									   older_than_type,
									   newer_than_type,
									   cascade,
									   cascades_to_materializations,
									   verbose);

	ts_dist_cmd_run_on_data_nodes(sql_cmd, data_node_names);
}
