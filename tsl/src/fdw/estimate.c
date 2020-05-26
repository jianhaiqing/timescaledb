/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <nodes/nodes.h>
#include <optimizer/cost.h>
#include <optimizer/clauses.h>
#include <optimizer/tlist.h>
#include <utils/selfuncs.h>
#include <utils/rel.h>
#include <lib/stringinfo.h>

#include <remote/connection.h>
#include <remote/async.h>
#include <remote/dist_txn.h>

#include "relinfo.h"
#include "estimate.h"
#include "deparse.h"

/* If no remote estimates, assume a sort costs 5% extra.  */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.05

typedef struct CostEstimate
{
	double rows;
	double retrieved_rows;
	int width;
	Cost startup_cost;
	Cost total_cost;
	Cost cpu_per_tuple;
	Cost run_cost;
} CostEstimate;

/*
 * Estimate costs of executing a SQL statement remotely.
 * The given "sql" must be an EXPLAIN command.
 */
static void
send_remote_estimate_query(const char *sql, TSConnection *conn, CostEstimate *ce)
{
	AsyncResponseResult *volatile rsp = NULL;

	/* PGresult must be released before leaving this function. */
	PG_TRY();
	{
		AsyncRequest *req;
		PGresult *res;
		char *line;
		char *p;
		int n;

		/*
		 * Execute EXPLAIN remotely.
		 */
		req = async_request_send(conn, sql);
		rsp = async_request_wait_any_result(req);
		res = async_response_result_get_pg_result(rsp);

		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			async_response_report_error((AsyncResponse *) rsp, ERROR);

		/*
		 * Extract cost numbers for topmost plan node.  Note we search for a
		 * left paren from the end of the line to avoid being confused by
		 * other uses of parentheses.
		 */
		line = PQgetvalue(res, 0, 0);
		p = strrchr(line, '(');
		if (p == NULL)
			elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);
		n = sscanf(p,
				   "(cost=%lf..%lf rows=%lf width=%d)",
				   &ce->startup_cost,
				   &ce->total_cost,
				   &ce->rows,
				   &ce->width);
		if (n != 4)
			elog(ERROR, "could not interpret EXPLAIN output: \"%s\"", line);

		async_response_result_close(rsp);
	}
	PG_CATCH();
	{
		if (NULL != rsp)
			async_response_result_close(rsp);

		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
get_remote_estimate(PlannerInfo *root, RelOptInfo *rel, List *param_join_conds, List *pathkeys,
					CostEstimate *ce)
{
	TsFdwRelInfo *fpinfo = fdw_relinfo_get(rel);
	List *remote_param_join_conds;
	List *local_param_join_conds;
	StringInfoData sql;
	TSConnection *conn;
	Selectivity local_sel;
	QualCost local_cost;
	List *fdw_scan_tlist = NIL;
	List *remote_conds;

	/* Required only to be passed to deparseSelectStmtForRel */
	List *retrieved_attrs;

	/*
	 * param_join_conds might contain both clauses that are safe to send
	 * across, and clauses that aren't.
	 */
	classify_conditions(root,
						rel,
						param_join_conds,
						&remote_param_join_conds,
						&local_param_join_conds);

	if (IS_JOIN_REL(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign joins are not supported")));

	/* Build the list of columns to be fetched from the data node. */
	if (IS_UPPER_REL(rel))
		fdw_scan_tlist = build_tlist_to_deparse(rel);
	else
		fdw_scan_tlist = NIL;

	/*
	 * The complete list of remote conditions includes everything from
	 * baserestrictinfo plus any extra join_conds relevant to this
	 * particular path.
	 */
	remote_conds = list_concat(list_copy(remote_param_join_conds), fpinfo->remote_conds);

	/*
	 * Construct EXPLAIN query including the desired SELECT, FROM, and
	 * WHERE clauses. Params and other-relation Vars are replaced by dummy
	 * values, so don't request params_list.
	 */
	initStringInfo(&sql);
	appendStringInfoString(&sql, "EXPLAIN ");
	deparseSelectStmtForRel(&sql,
							root,
							rel,
							fdw_scan_tlist,
							remote_conds,
							pathkeys,
							false,
							&retrieved_attrs,
							NULL,
							fpinfo->sca,
							NULL);

	/* Get the remote estimate */
	conn = remote_dist_txn_get_connection(fpinfo->cid, REMOTE_TXN_NO_PREP_STMT);
	send_remote_estimate_query(sql.data, conn, ce);

	ce->retrieved_rows = ce->rows;

	/* Factor in the selectivity of the locally-checked quals */
	local_sel = clauselist_selectivity(root, local_param_join_conds, rel->relid, JOIN_INNER, NULL);
	local_sel *= fpinfo->local_conds_sel;

	ce->rows = clamp_row_est(ce->rows * local_sel);

	/* Add in the eval cost of the locally-checked quals */
	ce->startup_cost += fpinfo->local_conds_cost.startup;
	ce->total_cost += fpinfo->local_conds_cost.per_tuple * ce->retrieved_rows;
	cost_qual_eval(&local_cost, local_param_join_conds, root);
	ce->startup_cost += local_cost.startup;
	ce->total_cost += local_cost.per_tuple * ce->retrieved_rows;
}

static void
get_upper_rel_estimate(PlannerInfo *root, RelOptInfo *rel, CostEstimate *ce)
{
	TsFdwRelInfo *fpinfo = fdw_relinfo_get(rel);
	TsFdwRelInfo *ofpinfo = fdw_relinfo_get(fpinfo->outerrel);
	PathTarget *ptarget = rel->reltarget;
	AggClauseCosts aggcosts;
	double input_rows;
	int num_group_cols;
	double num_groups = 1;

	/* Make sure the core code set the pathtarget. */
	Assert(ptarget != NULL);

	/*
	 * This cost model is mixture of costing done for sorted and
	 * hashed aggregates in cost_agg().  We are not sure which
	 * strategy will be considered at remote side, thus for
	 * simplicity, we put all startup related costs in startup_cost
	 * and all finalization and run cost are added in total_cost.
	 *
	 * Also, core does not care about costing HAVING expressions and
	 * adding that to the costs.  So similarly, here too we are not
	 * considering remote and local conditions for costing.
	 */

	/* Get rows and width from input rel */
	input_rows = ofpinfo->rows;
	ce->width = ofpinfo->width;

	/* Collect statistics about aggregates for estimating costs. */
	MemSet(&aggcosts, 0, sizeof(AggClauseCosts));

	if (root->parse->hasAggs)
	{
		get_agg_clause_costs(root, (Node *) fpinfo->grouped_tlist, AGGSPLIT_SIMPLE, &aggcosts);

		/*
		 * The cost of aggregates in the HAVING qual will be the same
		 * for each child as it is for the parent, so there's no need
		 * to use a translated version of havingQual.
		 */
		get_agg_clause_costs(root, (Node *) root->parse->havingQual, AGGSPLIT_SIMPLE, &aggcosts);
	}

	/* Get number of grouping columns and possible number of groups */
	num_group_cols = list_length(root->parse->groupClause);
	num_groups = estimate_num_groups(root,
									 get_sortgrouplist_exprs(root->parse->groupClause,
															 fpinfo->grouped_tlist),
									 input_rows,
									 NULL);

	/*
	 * Number of rows expected from data node will be same as
	 * that of number of groups.
	 */
	ce->rows = ce->retrieved_rows = num_groups;

	/*-----
	 * Startup cost includes:
	 *	  1. Startup cost for underneath input * relation
	 *	  2. Cost of performing aggregation, per cost_agg()
	 *	  3. Startup cost for PathTarget eval
	 *-----
	 */
	ce->startup_cost = ofpinfo->rel_startup_cost;
	ce->startup_cost += aggcosts.transCost.startup;
	ce->startup_cost += aggcosts.transCost.per_tuple * input_rows;
	ce->startup_cost += (cpu_operator_cost * num_group_cols) * input_rows;
	ce->startup_cost += ptarget->cost.startup;

	/*-----
	 * Run time cost includes:
	 *	  1. Run time cost of underneath input relation
	 *	  2. Run time cost of performing aggregation, per cost_agg()
	 *	  3. PathTarget eval cost for each output row
	 *-----
	 */
	ce->run_cost = ofpinfo->rel_total_cost - ofpinfo->rel_startup_cost;
#if PG12_GE
	ce->run_cost += aggcosts.finalCost.per_tuple * num_groups;
#else
	ce->run_cost += aggcosts.finalCost * num_groups;
#endif
	ce->run_cost += cpu_tuple_cost * num_groups;
	ce->run_cost += ptarget->cost.per_tuple * num_groups;
}

static void
get_base_rel_estimate(PlannerInfo *root, RelOptInfo *rel, CostEstimate *ce)
{
	TsFdwRelInfo *fpinfo = fdw_relinfo_get(rel);

	/* Back into an estimate of the number of retrieved rows. */
	ce->retrieved_rows = clamp_row_est(rel->rows / fpinfo->local_conds_sel);

	/* Clamp retrieved rows estimates to at most rel->tuples. */
	ce->retrieved_rows = Min(ce->retrieved_rows, rel->tuples);

	/*
	 * Cost as though this were a seqscan, which is pessimistic.  We
	 * effectively imagine the local_conds are being evaluated
	 * remotely, too.
	 */
	ce->startup_cost = 0;
	ce->run_cost = 0;
	ce->run_cost += seq_page_cost * rel->pages;

	ce->startup_cost += rel->baserestrictcost.startup;
	ce->cpu_per_tuple = cpu_tuple_cost + rel->baserestrictcost.per_tuple;
	ce->run_cost += ce->cpu_per_tuple * rel->tuples;
}

#define REL_HAS_CACHED_COSTS(fpinfo)                                                               \
	((fpinfo)->rel_startup_cost >= 0 && (fpinfo)->rel_total_cost >= 0 &&                           \
	 (fpinfo)->rel_retrieved_rows >= 0)

/*
 * fdw_estimate_path_cost_size
 *		Get cost and size estimates for a foreign scan on given foreign
 *		relation either a base relation or an upper relation containing
 *		foreign relations.
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 *
 * The function returns the cost and size estimates in p_row, p_width,
 * p_startup_cost and p_total_cost variables.
 */
void
fdw_estimate_path_cost_size(PlannerInfo *root, RelOptInfo *rel, List *param_join_conds,
							List *pathkeys, double *p_rows, int *p_width, Cost *p_startup_cost,
							Cost *p_total_cost)
{
	TsFdwRelInfo *fpinfo = fdw_relinfo_get(rel);
	CostEstimate ce = {
		/*
		 * Use rows/width estimates made by set_baserel_size_estimates() for
		 * base foreign relations.
		 */
		.rows = rel->rows,
		.width = rel->reltarget->width,
	};

	if (IS_JOIN_REL(rel))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign joins are not supported")));

	/*
	 * If the table or the data node is configured to use remote estimates,
	 * connect to the data node and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction+join clauses.  Otherwise,
	 * estimate rows using whatever statistics we have locally, in a way
	 * similar to ordinary tables.
	 */
	if (fpinfo->use_remote_estimate)
		get_remote_estimate(root, rel, param_join_conds, pathkeys, &ce);
	else
	{
		/*
		 * We don't support join conditions in this mode (hence, no
		 * parameterized paths can be made).
		 */
		Assert(param_join_conds == NIL);

		/*
		 * We will come here again and again with different set of pathkeys
		 * that caller wants to cost. We don't need to calculate the cost of
		 * bare scan each time. Instead, use the costs if we have cached them
		 * already.
		 */
		if (REL_HAS_CACHED_COSTS(fpinfo))
		{
			ce.startup_cost = fpinfo->rel_startup_cost;
			ce.run_cost = fpinfo->rel_total_cost - fpinfo->rel_startup_cost;
			ce.retrieved_rows = fpinfo->rel_retrieved_rows;
		}
		else if (IS_UPPER_REL(rel))
			get_upper_rel_estimate(root, rel, &ce);
		else
			get_base_rel_estimate(root, rel, &ce);

		/*
		 * Without remote estimates, we have no real way to estimate the cost
		 * of generating sorted output.  It could be free if the query plan
		 * the remote side would have chosen generates properly-sorted output
		 * anyway, but in most cases it will cost something.  Estimate a value
		 * high enough that we won't pick the sorted path when the ordering
		 * isn't locally useful, but low enough that we'll err on the side of
		 * pushing down the ORDER BY clause when it's useful to do so.
		 */
		if (pathkeys != NIL)
		{
			/* TODO: check if sort covered by local index and use other sort multiplier */
			ce.startup_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
			ce.run_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
		}

		ce.total_cost = ce.startup_cost + ce.run_cost;
	}

	/*
	 * Cache the costs for scans without any pathkeys or parameterization
	 * before adding the costs for transferring data from the data node.
	 * These costs are useful for costing the join between this relation and
	 * another foreign relation or to calculate the costs of paths with
	 * pathkeys for this relation, when the costs can not be obtained from the
	 * data node. This function will be called at least once for every
	 * foreign relation without pathkeys and parameterization.
	 */
	if (!REL_HAS_CACHED_COSTS(fpinfo) && pathkeys == NIL && param_join_conds == NIL)
	{
		fpinfo->rel_startup_cost = ce.startup_cost;
		fpinfo->rel_total_cost = ce.total_cost;
		fpinfo->rel_retrieved_rows = ce.retrieved_rows;
	}

	/*
	 * Add some additional cost factors to account for connection overhead
	 * (fdw_startup_cost), transferring data across the network
	 * (fdw_tuple_cost per retrieved row), and local manipulation of the data
	 * (cpu_tuple_cost per retrieved row).
	 */
	ce.startup_cost += fpinfo->fdw_startup_cost;
	ce.total_cost += fpinfo->fdw_startup_cost;
	ce.total_cost += fpinfo->fdw_tuple_cost * ce.retrieved_rows;
	ce.total_cost += cpu_tuple_cost * ce.retrieved_rows;

	/* Return results. */
	*p_rows = ce.rows;
	*p_width = ce.width;
	*p_startup_cost = ce.startup_cost;
	*p_total_cost = ce.total_cost;
}
