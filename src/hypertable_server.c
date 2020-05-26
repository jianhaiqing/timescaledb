/*
 * This file and its contents are licensed under the Apache License 2.0.
 * Please see the included NOTICE for copyright information and
 * LICENSE-APACHE for a copy of the license.
 */
#include <postgres.h>
#include <utils/fmgroids.h>
#include <utils/builtins.h>
#include <utils/array.h>
#include <foreign/foreign.h>

#include "hypertable_server.h"
#include "scanner.h"
#include "catalog.h"

static void
hypertable_server_insert_relation(Relation rel, int32 hypertable_id, int32 server_hypertable_id,
								  Name server_name)
{
	TupleDesc desc = RelationGetDescr(rel);
	Datum values[Natts_hypertable_server];
	bool nulls[Natts_hypertable_server] = { false };
	CatalogSecurityContext sec_ctx;

	values[AttrNumberGetAttrOffset(Anum_hypertable_server_hypertable_id)] =
		Int32GetDatum(hypertable_id);
	values[AttrNumberGetAttrOffset(Anum_hypertable_server_server_name)] = NameGetDatum(server_name);

	if (server_hypertable_id > 0)
		values[AttrNumberGetAttrOffset(Anum_hypertable_server_server_hypertable_id)] =
			Int32GetDatum(server_hypertable_id);
	else
		nulls[AttrNumberGetAttrOffset(Anum_hypertable_server_server_hypertable_id)] = true;

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_insert_values(rel, desc, values, nulls);
	ts_catalog_restore_user(&sec_ctx);
}

TSDLLEXPORT void
ts_hypertable_server_insert_multi(List *hypertable_servers)
{
	Catalog *catalog = ts_catalog_get();
	Relation rel;
	ListCell *lc;

	rel = table_open(catalog->tables[HYPERTABLE_SERVER].id, RowExclusiveLock);

	foreach (lc, hypertable_servers)
	{
		HypertableServer *server = lfirst(lc);

		hypertable_server_insert_relation(rel,
										  server->fd.hypertable_id,
										  server->fd.server_hypertable_id,
										  &server->fd.server_name);
	}

	table_close(rel, RowExclusiveLock);
}

static int
hypertable_server_scan_limit_internal(ScanKeyData *scankey, int num_scankeys, int indexid,
									  tuple_found_func on_tuple_found, void *scandata, int limit,
									  LOCKMODE lock, MemoryContext mctx)
{
	Catalog *catalog = ts_catalog_get();
	ScannerCtx scanctx = {
		.table = catalog->tables[HYPERTABLE_SERVER].id,
		.index = catalog_get_index(catalog, HYPERTABLE_SERVER, indexid),
		.nkeys = num_scankeys,
		.scankey = scankey,
		.data = scandata,
		.limit = limit,
		.tuple_found = on_tuple_found,
		.lockmode = lock,
		.scandirection = ForwardScanDirection,
		.result_mctx = mctx,
	};

	return ts_scanner_scan(&scanctx);
}

static ScanTupleResult
hypertable_server_tuple_delete(TupleInfo *ti, void *data)
{
	CatalogSecurityContext sec_ctx;

	ts_catalog_database_info_become_owner(ts_catalog_database_info_get(), &sec_ctx);
	ts_catalog_delete(ti->scanrel, ti->tuple);
	ts_catalog_restore_user(&sec_ctx);

	return SCAN_CONTINUE;
}

static ScanTupleResult
hypertable_server_tuple_found(TupleInfo *ti, void *data)
{
	List **servers = data;
	const char *servername;
	HypertableServer *hypertable_server;
	ForeignServer *foreign_server;
	MemoryContext old;
	Datum values[Natts_hypertable_server];
	bool nulls[Natts_hypertable_server] = { false };

	/*
	 * Need to use heap_deform_tuple instead of GETSTRUCT since the tuple can
	 * contain NULL values
	 */
	heap_deform_tuple(ti->tuple, ti->desc, values, nulls);

	servername =
		DatumGetCString(values[AttrNumberGetAttrOffset(Anum_hypertable_server_server_name)]);
	foreign_server = GetForeignServerByName(servername, false);

	old = MemoryContextSwitchTo(ti->mctx);
	hypertable_server = palloc(sizeof(HypertableServer));

	hypertable_server->fd.hypertable_id =
		DatumGetInt32(values[AttrNumberGetAttrOffset(Anum_hypertable_server_hypertable_id)]);
	namestrcpy(&hypertable_server->fd.server_name, servername);

	if (nulls[AttrNumberGetAttrOffset(Anum_hypertable_server_server_hypertable_id)])
		hypertable_server->fd.server_hypertable_id = 0;
	else
		hypertable_server->fd.server_hypertable_id = DatumGetInt32(
			values[AttrNumberGetAttrOffset(Anum_hypertable_server_server_hypertable_id)]);

	hypertable_server->foreign_server_oid = foreign_server->serverid;
	*servers = lappend(*servers, hypertable_server);
	MemoryContextSwitchTo(old);

	return SCAN_CONTINUE;
}

static int
hypertable_server_scan_by_hypertable_id(int32 hypertable_id, tuple_found_func tuple_found,
										void *data, LOCKMODE lockmode, MemoryContext mctx)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0],
				Anum_hypertable_server_hypertable_id_server_name_idx_hypertable_id,
				BTEqualStrategyNumber,
				F_INT4EQ,
				Int32GetDatum(hypertable_id));

	return hypertable_server_scan_limit_internal(scankey,
												 1,
												 HYPERTABLE_SERVER_HYPERTABLE_ID_SERVER_NAME_IDX,
												 tuple_found,
												 data,
												 0,
												 lockmode,
												 mctx);
}

static int
hypertable_server_scan_by_servername(const char *servername, tuple_found_func tuple_found,
									 void *data, LOCKMODE lockmode, MemoryContext mctx)
{
	ScanKeyData scankey[1];

	ScanKeyInit(&scankey[0],
				Anum_hypertable_server_server_name,
				BTEqualStrategyNumber,
				F_NAMEEQ,
				DirectFunctionCall1(namein, CStringGetDatum(servername)));

	return hypertable_server_scan_limit_internal(scankey,
												 1,
												 INVALID_INDEXID,
												 tuple_found,
												 data,
												 0,
												 lockmode,
												 mctx);
}

TSDLLEXPORT List *
ts_hypertable_server_scan(int32 hypertable_id, MemoryContext mctx)
{
	List *hypertable_servers = NIL;

	hypertable_server_scan_by_hypertable_id(hypertable_id,
											hypertable_server_tuple_found,
											&hypertable_servers,
											AccessShareLock,
											mctx);

	return hypertable_servers;
}

TSDLLEXPORT int
ts_hypertable_server_delete_by_hypertable_id(int32 hypertable_id)
{
	return hypertable_server_scan_by_hypertable_id(hypertable_id,
												   hypertable_server_tuple_delete,
												   NULL,
												   RowExclusiveLock,
												   CurrentMemoryContext);
}

TSDLLEXPORT int
ts_hypertable_server_delete_by_servername(const char *servername)
{
	return hypertable_server_scan_by_servername(servername,
												hypertable_server_tuple_delete,
												NULL,
												RowExclusiveLock,
												CurrentMemoryContext);
}
