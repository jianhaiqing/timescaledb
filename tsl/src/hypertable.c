/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <catalog/pg_type.h>
#include <catalog/pg_proc.h>
#include <utils/builtins.h>
#include <utils/lsyscache.h>
#include <utils/syscache.h>
#include <utils/array.h>
#include <miscadmin.h>
#include <access/xact.h>
#include <cache.h>
#include <nodes/pg_list.h>

#include <dimension.h>
#include <errors.h>

#include "errors.h"
#include "hypertable.h"
#include "license.h"
#include "utils.h"
#include "hypertable_cache.h"

#if PG11_GE
#include <foreign/foreign.h>
#include <libpq-fe.h>

#include <hypertable_data_node.h>

#include "fdw/fdw.h"
#include "data_node.h"
#include "deparse.h"
#include "remote/dist_commands.h"
#include "compat.h"
#include "hypertable_data_node.h"
#include <extension.h>
#endif /*  PG11_GE */

Datum
hypertable_valid_ts_interval(PG_FUNCTION_ARGS)
{
	/* this function does all the necessary validation and if successfull,
	returns the interval which is not necessary here */
	ts_interval_from_tuple(PG_GETARG_DATUM(0));
	PG_RETURN_BOOL(true);
}

#if PG_VERSION_SUPPORTS_MULTINODE

static List *
data_node_append(List *data_nodes, int32 hypertable_id, const char *node_name,
				 int32 node_hypertable_id, bool block_chunks)
{
	ForeignServer *server = data_node_get_foreign_server(node_name, ACL_NO_CHECK, false);
	HypertableDataNode *hdn = palloc0(sizeof(HypertableDataNode));

	hdn->fd.hypertable_id = hypertable_id;
	namestrcpy(&hdn->fd.node_name, node_name);
	hdn->fd.node_hypertable_id = node_hypertable_id;
	hdn->foreign_server_oid = server->serverid;
	hdn->fd.block_chunks = block_chunks;

	return lappend(data_nodes, hdn);
}

/*  Returns the remote hypertable ids for the data_nodes (in the same order)
 */
static List *
hypertable_create_backend_tables(int32 hypertable_id, List *data_nodes)
{
	Hypertable *ht = ts_hypertable_get_by_id(hypertable_id);
	ListCell *cell;
	List *remote_ids = NIL;
	DistCmdResult *dist_res;
	DeparsedHypertableCommands *commands = deparse_get_distributed_hypertable_create_command(ht);

	foreach (cell, deparse_get_tabledef_commands(ht->main_table_relid))
		ts_dist_cmd_run_on_data_nodes(lfirst(cell), data_nodes);

	dist_res = ts_dist_cmd_invoke_on_data_nodes(commands->table_create_command, data_nodes, true);
	foreach (cell, data_nodes)
	{
		PGresult *res = ts_dist_cmd_get_result_by_node_name(dist_res, lfirst(cell));

		Assert(PQntuples(res) == 1);
		Assert(PQnfields(res) == AttrNumberGetAttrOffset(_Anum_create_hypertable_max));
		remote_ids =
			lappend(remote_ids,
					(void *) Int32GetDatum(atoi(
						PQgetvalue(res, 0, AttrNumberGetAttrOffset(Anum_create_hypertable_id)))));
	}
	ts_dist_cmd_close_response(dist_res);

	foreach (cell, commands->dimension_add_commands)
		ts_dist_cmd_run_on_data_nodes(lfirst(cell), data_nodes);

	return remote_ids;
}

/*
 * Assign data nodes to a hypertable.
 *
 * Given a list of data node names, add mappings to ensure the
 * hypertable is distributed across those nodes.
 *
 * Returns a list of HypertableDataNode objects that correspond to the given
 * data node names.
 */
List *
hypertable_assign_data_nodes(int32 hypertable_id, List *nodes)
{
	ListCell *lc;
	List *assigned_nodes = NIL;
	List *remote_ids = hypertable_create_backend_tables(hypertable_id, nodes);
	ListCell *id_cell;

	Assert(nodes->length == remote_ids->length);
	forboth (lc, nodes, id_cell, remote_ids)
	{
		assigned_nodes =
			data_node_append(assigned_nodes, hypertable_id, lfirst(lc), lfirst_int(id_cell), false);
	}

	ts_hypertable_data_node_insert_multi(assigned_nodes);

	return assigned_nodes;
}

void
hypertable_make_distributed(Hypertable *ht, ArrayType *data_nodes)
{
	List *nodelist;
	int num_nodes;

	/* Get the list of servers to attach to the distributed hypertable. We
	 * require USAGE on the servers to be able to attach them to the
	 * hypertable. */
	if (NULL == data_nodes)
		nodelist = data_node_get_node_name_list_with_aclcheck(ACL_USAGE);
	else
		nodelist = data_node_array_to_node_name_list_with_aclcheck(data_nodes, ACL_USAGE);

	num_nodes = list_length(nodelist);

	if (num_nodes == 0)
		ereport(ERROR,
				(errcode(ERRCODE_TS_NO_DATA_NODES),
				 errmsg("no data nodes can be assigned to \"%s\"",
						get_rel_name(ht->main_table_relid)),
				 errhint("Add data nodes using the add_data_node() function.")));

	hypertable_assign_data_nodes(ht->fd.id, nodelist);
}

#endif /* PG_VERSION_SUPPORTS_MULTINODE */
