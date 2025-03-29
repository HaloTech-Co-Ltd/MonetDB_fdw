/*-------------------------------------------------------------------------
 *
 * monetdb_fdw.c
 *		  Foreign-data wrapper for remote MonetDB databases
 *
 * Portions Copyright (c) 2025, Halo Tech Co.,Ltd. All rights reserved.
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 *
 * Author: zengman <zengman@halodbtech.com>
 * 
 * IDENTIFICATION
 *		  monetdb_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mapi.h>
#include <limits.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "access/table.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_foreign_server.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "executor/execAsync.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/inherit.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "monetdb_fdw.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/sampling.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/ruleutils.h"

PG_MODULE_MAGIC;

/* Default CPU cost to start up a foreign query. */
#define DEFAULT_FDW_STARTUP_COST	100.0

/* Default CPU cost to process 1 row (above and beyond cpu_tuple_cost). */
#define DEFAULT_FDW_TUPLE_COST		0.01

/* If no remote estimates, assume a sort costs 20% extra */
#define DEFAULT_FDW_SORT_MULTIPLIER 1.2

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * These items are indexed with the enum FdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the SELECT statement:
 *		sql = strVal(list_nth(fdw_private, FdwScanPrivateSelectSql));
 */
enum FdwScanPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwScanPrivateSelectSql,
	/* Integer list of attribute numbers retrieved by the SELECT */
	FdwScanPrivateRetrievedAttrs,
	/* Integer representing the desired fetch_size */
	FdwScanPrivateFetchSize,

	/*
	 * String describing join i.e. names of relations being joined and types
	 * of join, added when the scan is join
	 */
	FdwScanPrivateRelations
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ModifyTable node referencing a monetdb_fdw foreign table.  We store:
 *
 * 1) INSERT/UPDATE/DELETE statement text to be sent to the remote server
 * 2) Integer list of target attribute numbers for INSERT/UPDATE
 *	  (NIL for a DELETE)
 * 3) Length till the end of VALUES clause for INSERT
 *	  (-1 for a DELETE/UPDATE)
 * 4) Boolean flag showing if the remote query has a RETURNING clause
 * 5) Integer list of attribute numbers retrieved by RETURNING, if any
 */
enum FdwModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwModifyPrivateUpdateSql,
	/* Integer list of target attribute numbers for INSERT/UPDATE */
	FdwModifyPrivateTargetAttnums,
	/* Length till the end of VALUES clause (as an Integer node) */
	FdwModifyPrivateLen,
	/* has-returning flag (as a Boolean node) */
	FdwModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING */
	FdwModifyPrivateRetrievedAttrs
};

/*
 * Similarly, this enum describes what's kept in the fdw_private list for
 * a ForeignScan node that modifies a foreign table directly.  We store:
 *
 * 1) UPDATE/DELETE statement text to be sent to the remote server
 * 2) Boolean flag showing if the remote query has a RETURNING clause
 * 3) Integer list of attribute numbers retrieved by RETURNING, if any
 * 4) Boolean flag showing if we set the command es_processed
 */
enum FdwDirectModifyPrivateIndex
{
	/* SQL statement to execute remotely (as a String node) */
	FdwDirectModifyPrivateUpdateSql,
	/* has-returning flag (as a Boolean node) */
	FdwDirectModifyPrivateHasReturning,
	/* Integer list of attribute numbers retrieved by RETURNING */
	FdwDirectModifyPrivateRetrievedAttrs,
	/* set-processed flag (as a Boolean node) */
	FdwDirectModifyPrivateSetProcessed
};

/*
 * Execution state of a foreign scan using monetdb_fdw.
 */
typedef struct MonetdbFdwScanState
{
	Relation	rel;			/* relcache entry for the foreign table. NULL
								 * for a foreign join scan. */
	TupleDesc	tupdesc;		/* tuple descriptor of scan */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* extracted fdw_private data */
	char	   *query;			/* text of SELECT command */
	List	   *retrieved_attrs;	/* list of retrieved attribute numbers */

	/* for remote query execution */
	Mapi 	   conn;			/* connection for the scan */
	MapiHdl 	hdl;
	int			numParams;		/* number of parameters passed to query */
	FmgrInfo   *param_flinfo;	/* output conversion functions for them */
	List	   *param_exprs;	/* executable expressions for param values */
	const char **param_values;	/* textual values of query parameters */

	/* for storing result tuples */
	HeapTuple  *tuples;			/* array of currently-retrieved tuples */
	int			num_tuples;		/* # of tuples in array */
	int			next_tuple;		/* index of next one to return */

	/* batch-level state, for optimizing rewinds and avoiding useless fetch */
	int			fetch_ct_2;		/* Min(# of fetches done, 2) */
	bool		eof_reached;	/* true if last fetch reached EOF */

	/* for asynchronous execution */
	bool		async_capable;	/* engage asynchronous-capable logic? */

	/* working memory contexts */
	MemoryContext batch_cxt;	/* context holding current batch of tuples */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */

	int			fetch_size;		/* number of tuples per fetch */
} MonetdbFdwScanState;

/*
 * Execution state of a foreign insert/update/delete operation.
 */
typedef struct MonetdbFdwModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	AttInMetadata *attinmeta;	/* attribute datatype conversion metadata */

	/* for remote query execution */
	Mapi 	   conn;			/* connection for the scan */
	MapiHdl 	hdl;
	char	   *p_name;			/* name of prepared statement, if created */

	/* extracted fdw_private data */
	char	   *query;			/* text of INSERT/UPDATE/DELETE command */
	char	   *orig_query;		/* original text of INSERT command */
	List	   *target_attrs;	/* list of target attribute numbers */
	int			values_end;		/* length up to the end of VALUES */
	int			batch_size;		/* value of FDW option "batch_size" */
	bool		has_returning;	/* is there a RETURNING clause? */
	List	   *retrieved_attrs;	/* attr numbers retrieved by RETURNING */

	/* info about parameters for prepared statement */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */
	List	   *key_attnums;		/* attnum of input resjunk key column */
	FmgrInfo   *key_flinfo;
	/* batch operation stuff */
	int			num_slots;		/* number of slots to insert */

	/* working memory context */
	MemoryContext temp_cxt;		/* context for per-tuple temporary data */

	/* for update row movement if subplan result rel */
	struct MonetdbFdwModifyState *aux_fmstate;	/* foreign-insert state, if
											 * created */
} MonetdbFdwModifyState;

/*
 * This enum describes what's kept in the fdw_private list for a ForeignPath.
 * We store:
 *
 * 1) Boolean flag showing if the remote query has the final sort
 * 2) Boolean flag showing if the remote query has the LIMIT clause
 */
enum FdwPathPrivateIndex
{
	/* has-final-sort flag (as a Boolean node) */
	FdwPathPrivateHasFinalSort,
	/* has-limit flag (as a Boolean node) */
	FdwPathPrivateHasLimit
};

/* Struct for extra information passed to estimate_path_cost_size() */
typedef struct
{
	PathTarget *target;
	bool		has_final_sort;
	bool		has_limit;
	double		limit_tuples;
	int64		count_est;
	int64		offset_est;
} MonetdbFdwPathExtraData;

/*
 * Identify the attribute where data conversion fails.
 */
typedef struct ConversionLocation
{
	AttrNumber	cur_attno;		/* attribute number being processed, or 0 */
	Relation	rel;			/* foreign table being processed, or NULL */
	ForeignScanState *fsstate;	/* plan node being processed, or NULL */
} ConversionLocation;

/* Callback argument for ec_member_matches_foreign */
typedef struct
{
	Expr	   *current;		/* current expr, or NULL if not yet found */
	List	   *already_used;	/* expressions already dealt with */
} ec_member_foreign_arg;

/*
 * SQL functions
 */
PG_FUNCTION_INFO_V1(monetdb_fdw_handler);
PG_FUNCTION_INFO_V1(monetdb_execute);

/*
 * FDW callback routines
 */
static void MonetDB_GetForeignRelSize(PlannerInfo *root,
									  RelOptInfo *baserel,
									  Oid foreigntableid);
static void MonetDB_GetForeignPaths(PlannerInfo *root,
									RelOptInfo *baserel,
									Oid foreigntableid);
static ForeignScan *MonetDB_GetForeignPlan(PlannerInfo *root,
										   RelOptInfo *foreignrel,
										   Oid foreigntableid,
										   ForeignPath *best_path,
										   List *tlist,
										   List *scan_clauses,
										   Plan *outer_plan);
static void MonetDB_BeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *MonetDB_IterateForeignScan(ForeignScanState *node);
static void MonetDB_ReScanForeignScan(ForeignScanState *node);
static void MonetDB_EndForeignScan(ForeignScanState *node);
static void MonetDB_AddForeignUpdateTargets(PlannerInfo *root,
											Index rtindex,
											RangeTblEntry *target_rte,
											Relation target_relation);
static List *MonetDB_PlanForeignModify(PlannerInfo *root,
									   ModifyTable *plan,
									   Index resultRelation,
									   int subplan_index);
static void MonetDB_BeginForeignModify(ModifyTableState *mtstate,
									   ResultRelInfo *resultRelInfo,
									   List *fdw_private,
									   int subplan_index,
									   int eflags);
static TupleTableSlot *MonetDB_ExecForeignInsert(EState *estate,
												 ResultRelInfo *resultRelInfo,
												 TupleTableSlot *slot,
												 TupleTableSlot *planSlot);
static TupleTableSlot **MonetDB_ExecForeignBatchInsert(EState *estate,
													   ResultRelInfo *resultRelInfo,
													   TupleTableSlot **slots,
													   TupleTableSlot **planSlots,
													   int *numSlots);
static TupleTableSlot *MonetDB_ExecForeignUpdate(EState *estate,
												 ResultRelInfo *resultRelInfo,
												 TupleTableSlot *slot,
												 TupleTableSlot *planSlot);
static TupleTableSlot *MonetDB_ExecForeignDelete(EState *estate,
												 ResultRelInfo *resultRelInfo,
												 TupleTableSlot *slot,
												 TupleTableSlot *planSlot);
static void MonetDB_EndForeignModify(EState *estate,
									 ResultRelInfo *resultRelInfo);
static void MonetDB_BeginForeignInsert(ModifyTableState *mtstate,
									   ResultRelInfo *resultRelInfo);
static void MonetDB_EndForeignInsert(EState *estate,
									 ResultRelInfo *resultRelInfo);
static int	MonetDB_IsForeignRelUpdatable(Relation rel);
static void MonetDB_BeginDirectModify(ForeignScanState *node, int eflags);
static TupleTableSlot *MonetDB_IterateDirectModify(ForeignScanState *node);
static void MonetDB_EndDirectModify(ForeignScanState *node);
static void MonetDB_ExplainForeignScan(ForeignScanState *node,
									   ExplainState *es);
static void MonetDB_ExplainForeignModify(ModifyTableState *mtstate,
										 ResultRelInfo *rinfo,
										 List *fdw_private,
										 int subplan_index,
										 ExplainState *es);
static void MonetDB_ExplainDirectModify(ForeignScanState *node,
										ExplainState *es);
static void MonetDB_ExecForeignTruncate(List *rels,
										DropBehavior behavior,
										bool restart_seqs);
static bool MonetDB_AnalyzeForeignTable(Relation relation,
										AcquireSampleRowsFunc *func,
										BlockNumber *totalpages);
static List *MonetDB_ImportForeignSchema(ImportForeignSchemaStmt *stmt,
										 Oid serverOid);
static void MonetDB_GetForeignJoinPaths(PlannerInfo *root,
										RelOptInfo *joinrel,
										RelOptInfo *outerrel,
										RelOptInfo *innerrel,
										JoinType jointype,
										JoinPathExtraData *extra);
static bool MonetDB_RecheckForeignScan(ForeignScanState *node,
									   TupleTableSlot *slot);
static void MonetDB_GetForeignUpperPaths(PlannerInfo *root,
										 UpperRelationKind stage,
										 RelOptInfo *input_rel,
										 RelOptInfo *output_rel,
										 void *extra);
static bool MonetDB_IsForeignPathAsyncCapable(ForeignPath *path);
static void MonetDB_ForeignAsyncRequest(AsyncRequest *areq);
static void MonetDB_ForeignAsyncConfigureWait(AsyncRequest *areq);
static void MonetDB_ForeignAsyncNotify(AsyncRequest *areq);

/*
 * Helper functions
 */
static void estimate_path_cost_size(PlannerInfo *root,
									RelOptInfo *foreignrel,
									List *param_join_conds,
									List *pathkeys,
									MonetdbFdwPathExtraData *fpextra,
									double *p_rows, int *p_width,
									Cost *p_startup_cost, Cost *p_total_cost);
static void adjust_foreign_grouping_path_cost(PlannerInfo *root,
											  List *pathkeys,
											  double retrieved_rows,
											  double width,
											  double limit_tuples,
											  Cost *p_startup_cost,
											  Cost *p_run_cost);
static bool ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
									  EquivalenceClass *ec, EquivalenceMember *em,
									  void *arg);
static void fetch_more_data(ForeignScanState *node);
static void prepare_query_params(PlanState *node,
								 List *fdw_exprs,
								 int numParams,
								 FmgrInfo **param_flinfo,
								 List **param_exprs,
								 const char ***param_values);
static HeapTuple make_tuple_from_result_row(MapiHdl res,
											int row,
											Relation rel,
											AttInMetadata *attinmeta,
											List *retrieved_attrs,
											ForeignScanState *fsstate,
											MemoryContext temp_context);
static void conversion_error_callback(void *arg);
static bool foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
								Node *havingQual);
static List *get_useful_pathkeys_for_relation(PlannerInfo *root,
											  RelOptInfo *rel);
static List *get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel);
static void add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
											Path *epq_path);
static void add_foreign_grouping_paths(PlannerInfo *root,
									   RelOptInfo *input_rel,
									   RelOptInfo *grouped_rel,
									   GroupPathExtraData *extra);
static void add_foreign_ordered_paths(PlannerInfo *root,
									  RelOptInfo *input_rel,
									  RelOptInfo *ordered_rel);
static void add_foreign_final_paths(PlannerInfo *root,
									RelOptInfo *input_rel,
									RelOptInfo *final_rel,
									FinalPathExtraData *extra);
static void apply_table_options(MonetdbFdwRelationInfo *fpinfo);
static void merge_fdw_options(MonetdbFdwRelationInfo *fpinfo,
							  const MonetdbFdwRelationInfo *fpinfo_o,
							  const MonetdbFdwRelationInfo *fpinfo_i);
static MonetdbFdwModifyState *create_foreign_modify(EState *estate,
													RangeTblEntry *rte,
													ResultRelInfo *resultRelInfo,
													CmdType operation,
													Plan *subplan,
													char *query,
													List *target_attrs,
													int values_end,
													bool has_returning,
													List *retrieved_attrs);
static TupleTableSlot **execute_foreign_modify(EState *estate,
												ResultRelInfo *resultRelInfo,
												CmdType operation,
												TupleTableSlot **slots,
												TupleTableSlot **planSlots,
												int *numSlots);
static const char **convert_prep_stmt_params(MonetdbFdwModifyState *fmstate,
													List *tupleid_keys,
													TupleTableSlot **slots,
													int numSlots);

/*
 * Foreign-data wrapper handler function: return a struct with pointers
 * to my callback routines.
 */
Datum
monetdb_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	/* Functions for scanning foreign tables */
	routine->GetForeignRelSize = MonetDB_GetForeignRelSize;
	routine->GetForeignPaths = MonetDB_GetForeignPaths;
	routine->GetForeignPlan = MonetDB_GetForeignPlan;
	routine->BeginForeignScan = MonetDB_BeginForeignScan;
	routine->IterateForeignScan = MonetDB_IterateForeignScan;
	routine->ReScanForeignScan = MonetDB_ReScanForeignScan;
	routine->EndForeignScan = MonetDB_EndForeignScan;

	/* Functions for updating foreign tables */
	routine->AddForeignUpdateTargets = MonetDB_AddForeignUpdateTargets;
	routine->PlanForeignModify = MonetDB_PlanForeignModify;
	routine->BeginForeignModify = MonetDB_BeginForeignModify;
	routine->ExecForeignInsert = MonetDB_ExecForeignInsert;
	routine->ExecForeignBatchInsert = MonetDB_ExecForeignBatchInsert;
	routine->GetForeignModifyBatchSize = NULL;
	routine->ExecForeignUpdate = MonetDB_ExecForeignUpdate;
	routine->ExecForeignDelete = MonetDB_ExecForeignDelete;
	routine->EndForeignModify = MonetDB_EndForeignModify;
	routine->BeginForeignInsert = MonetDB_BeginForeignInsert;
	routine->EndForeignInsert = MonetDB_EndForeignInsert;
	routine->IsForeignRelUpdatable = MonetDB_IsForeignRelUpdatable;
	routine->PlanDirectModify = NULL;
	routine->BeginDirectModify = MonetDB_BeginDirectModify;
	routine->IterateDirectModify = MonetDB_IterateDirectModify;
	routine->EndDirectModify = MonetDB_EndDirectModify;

	/* Function for EvalPlanQual rechecks */
	routine->RecheckForeignScan = MonetDB_RecheckForeignScan;
	/* Support functions for EXPLAIN */
	routine->ExplainForeignScan = MonetDB_ExplainForeignScan;
	routine->ExplainForeignModify = MonetDB_ExplainForeignModify;
	routine->ExplainDirectModify = MonetDB_ExplainDirectModify;

	/* Support function for TRUNCATE */
	routine->ExecForeignTruncate = MonetDB_ExecForeignTruncate;

	/* Support functions for ANALYZE */
	routine->AnalyzeForeignTable = MonetDB_AnalyzeForeignTable;

	/* Support functions for IMPORT FOREIGN SCHEMA */
	routine->ImportForeignSchema = MonetDB_ImportForeignSchema;

	/* Support functions for join push-down */
	routine->GetForeignJoinPaths = MonetDB_GetForeignJoinPaths;

	/* Support functions for upper relation push-down */
	routine->GetForeignUpperPaths = MonetDB_GetForeignUpperPaths;

	/* Support functions for asynchronous execution */
	routine->IsForeignPathAsyncCapable = MonetDB_IsForeignPathAsyncCapable;
	routine->ForeignAsyncRequest = MonetDB_ForeignAsyncRequest;
	routine->ForeignAsyncConfigureWait = MonetDB_ForeignAsyncConfigureWait;
	routine->ForeignAsyncNotify = MonetDB_ForeignAsyncNotify;

	PG_RETURN_POINTER(routine);
}

/*
 * MonetDB_GetForeignRelSize
 *		Estimate # of rows and width of the result of the scan
 *
 * We should consider the effect of all baserestrictinfo clauses here, but
 * not any join clauses.
 */
static void
MonetDB_GetForeignRelSize(PlannerInfo *root,
						  RelOptInfo *baserel,
						  Oid foreigntableid)
{
	MonetdbFdwRelationInfo *fpinfo;
	ListCell   *lc;

	/*
	 * We use MonetdbFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = (MonetdbFdwRelationInfo *) palloc0(sizeof(MonetdbFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;

	/* Base foreign tables need to be pushed down always. */
	fpinfo->pushdown_safe = true;

	/* Look up foreign-table catalog info. */
	fpinfo->table = GetForeignTable(foreigntableid);
	fpinfo->server = GetForeignServer(fpinfo->table->serverid);

	/*
	 * Extract user-settable option values.  Note that per-table settings of
	 * use_remote_estimate, fetch_size and async_capable override per-server
	 * settings of them, respectively.
	 */
	fpinfo->use_remote_estimate = false;
	fpinfo->fdw_startup_cost = DEFAULT_FDW_STARTUP_COST;
	fpinfo->fdw_tuple_cost = DEFAULT_FDW_TUPLE_COST;
	fpinfo->shippable_extensions = NIL;
	fpinfo->fetch_size = 100;
	fpinfo->async_capable = false;

	apply_table_options(fpinfo);

	/*
	 * If the table or the server is configured to use remote estimates,
	 * identify which user to do remote access as during planning.  This
	 * should match what ExecCheckPermissions() does.  If we fail due to lack
	 * of permissions, the query would have failed at runtime anyway.
	 */
	if (fpinfo->use_remote_estimate)
	{
		Oid			userid;

		userid = OidIsValid(baserel->userid) ? baserel->userid : GetUserId();
		fpinfo->user = GetUserMapping(userid, fpinfo->server->serverid);
	}
	else
		fpinfo->user = NULL;

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.
	 */
	classifyConditions(root, baserel, baserel->baserestrictinfo,
					   &fpinfo->remote_conds, &fpinfo->local_conds);

	/*
	 * Identify which attributes will need to be retrieved from the remote
	 * server.  These include all attrs needed for joins or final output, plus
	 * all attrs used in the local_conds.  (Note: if we end up using a
	 * parameterized scan, it's possible that some of the join clauses will be
	 * sent to the remote and thus we wouldn't really need to retrieve the
	 * columns used in them.  Doesn't seem worth detecting that case though.)
	 */
	fpinfo->attrs_used = NULL;
	pull_varattnos((Node *) baserel->reltarget->exprs, baserel->relid,
				   &fpinfo->attrs_used);
	foreach(lc, fpinfo->local_conds)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		pull_varattnos((Node *) rinfo->clause, baserel->relid,
					   &fpinfo->attrs_used);
	}

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  The best we can do for these
	 * conditions is to estimate selectivity on the basis of local statistics.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 baserel->relid,
													 JOIN_INNER,
													 NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction clauses, as well as the
	 * average row width.  Otherwise, estimate using whatever statistics we
	 * have locally, in a way similar to ordinary tables.
	 */
	if (fpinfo->use_remote_estimate)
	{
		/*
		 * Get cost/size estimates with help of remote server.  Save the
		 * values in fpinfo so we don't need to do it again to generate the
		 * basic foreign path.
		 */
		estimate_path_cost_size(root, baserel, NIL, NIL, NULL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->startup_cost, &fpinfo->total_cost);

		/* Report estimated baserel size to planner. */
		baserel->rows = fpinfo->rows;
		baserel->reltarget->width = fpinfo->width;
	}
	else
	{
		/*
		 * If the foreign table has never been ANALYZEd, it will have
		 * reltuples < 0, meaning "unknown".  We can't do much if we're not
		 * allowed to consult the remote server, but we can use a hack similar
		 * to plancat.c's treatment of empty relations: use a minimum size
		 * estimate of 10 pages, and divide by the column-datatype-based width
		 * estimate to get the corresponding number of tuples.
		 */
		if (baserel->tuples < 0)
		{
			baserel->pages = 10;
			baserel->tuples =
				(10 * BLCKSZ) / (baserel->reltarget->width +
								 MAXALIGN(SizeofHeapTupleHeader));
		}

		/* Estimate baserel size as best we can with local statistics. */
		set_baserel_size_estimates(root, baserel);

		/* Fill in basically-bogus cost estimates for use later. */
		estimate_path_cost_size(root, baserel, NIL, NIL, NULL,
								&fpinfo->rows, &fpinfo->width,
								&fpinfo->startup_cost, &fpinfo->total_cost);
	}

	/*
	 * fpinfo->relation_name gets the numeric rangetable index of the foreign
	 * table RTE.  (If this query gets EXPLAIN'd, we'll convert that to a
	 * human-readable string at that time.)
	 */
	fpinfo->relation_name = psprintf("%u", baserel->relid);

	/* No outer and inner relations. */
	fpinfo->make_outerrel_subquery = false;
	fpinfo->make_innerrel_subquery = false;
	fpinfo->lower_subquery_rels = NULL;
	/* Set the relation index. */
	fpinfo->relation_index = baserel->relid;
}

/*
 * get_useful_ecs_for_relation
 *		Determine which EquivalenceClasses might be involved in useful
 *		orderings of this relation.
 *
 * This function is in some respects a mirror image of the core function
 * pathkeys_useful_for_merging: for a regular table, we know what indexes
 * we have and want to test whether any of them are useful.  For a foreign
 * table, we don't know what indexes are present on the remote side but
 * want to speculate about which ones we'd like to use if they existed.
 *
 * This function returns a list of potentially-useful equivalence classes,
 * but it does not guarantee that an EquivalenceMember exists which contains
 * Vars only from the given relation.  For example, given ft1 JOIN t1 ON
 * ft1.x + t1.x = 0, this function will say that the equivalence class
 * containing ft1.x + t1.x is potentially useful.  Supposing ft1 is remote and
 * t1 is local (or on a different server), it will turn out that no useful
 * ORDER BY clause can be generated.  It's not our job to figure that out
 * here; we're only interested in identifying relevant ECs.
 */
static List *
get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_eclass_list = NIL;
	ListCell   *lc;
	Relids		relids;

	/*
	 * First, consider whether any active EC is potentially useful for a merge
	 * join against this relation.
	 */
	if (rel->has_eclass_joins)
	{
		foreach(lc, root->eq_classes)
		{
			EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc);

			if (eclass_useful_for_merging(root, cur_ec, rel))
				useful_eclass_list = lappend(useful_eclass_list, cur_ec);
		}
	}

	/*
	 * Next, consider whether there are any non-EC derivable join clauses that
	 * are merge-joinable.  If the joininfo list is empty, we can exit
	 * quickly.
	 */
	if (rel->joininfo == NIL)
		return useful_eclass_list;

	/* If this is a child rel, we must use the topmost parent rel to search. */
	if (IS_OTHER_REL(rel))
	{
		Assert(!bms_is_empty(rel->top_parent_relids));
		relids = rel->top_parent_relids;
	}
	else
		relids = rel->relids;

	/* Check each join clause in turn. */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/* Consider only mergejoinable clauses */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;

		/* Make sure we've got canonical ECs. */
		update_mergeclause_eclasses(root, restrictinfo);

		/*
		 * restrictinfo->mergeopfamilies != NIL is sufficient to guarantee
		 * that left_ec and right_ec will be initialized, per comments in
		 * distribute_qual_to_rels.
		 *
		 * We want to identify which side of this merge-joinable clause
		 * contains columns from the relation produced by this RelOptInfo. We
		 * test for overlap, not containment, because there could be extra
		 * relations on either side.  For example, suppose we've got something
		 * like ((A JOIN B ON A.x = B.x) JOIN C ON A.y = C.y) LEFT JOIN D ON
		 * A.y = D.y.  The input rel might be the joinrel between A and B, and
		 * we'll consider the join clause A.y = D.y. relids contains a
		 * relation not involved in the join class (B) and the equivalence
		 * class for the left-hand side of the clause contains a relation not
		 * involved in the input rel (C).  Despite the fact that we have only
		 * overlap and not containment in either direction, A.y is potentially
		 * useful as a sort column.
		 *
		 * Note that it's even possible that relids overlaps neither side of
		 * the join clause.  For example, consider A LEFT JOIN B ON A.x = B.x
		 * AND A.x = 1.  The clause A.x = 1 will appear in B's joininfo list,
		 * but overlaps neither side of B.  In that case, we just skip this
		 * join clause, since it doesn't suggest a useful sort order for this
		 * relation.
		 */
		if (bms_overlap(relids, restrictinfo->right_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->right_ec);
		else if (bms_overlap(relids, restrictinfo->left_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->left_ec);
	}

	return useful_eclass_list;
}

/*
 * get_useful_pathkeys_for_relation
 *		Determine which orderings of a relation might be useful.
 *
 * Getting data in sorted order can be useful either because the requested
 * order matches the final output ordering for the overall query we're
 * planning, or because it enables an efficient merge join.  Here, we try
 * to figure out which pathkeys to consider.
 */
static List *
get_useful_pathkeys_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_eclass_list;
	MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) rel->fdw_private;
	EquivalenceClass *query_ec = NULL;
	ListCell   *lc;

	/*
	 * Pushing the query_pathkeys to the remote server is always worth
	 * considering, because it might let us avoid a local sort.
	 */
	fpinfo->qp_is_pushdown_safe = false;
	if (root->query_pathkeys)
	{
		bool		query_pathkeys_ok = true;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);

			/*
			 * The planner and executor don't have any clever strategy for
			 * taking data sorted by a prefix of the query's pathkeys and
			 * getting it to be sorted by all of those pathkeys. We'll just
			 * end up resorting the entire data set.  So, unless we can push
			 * down all of the query pathkeys, forget it.
			 */
			if (!is_foreign_pathkey(root, rel, pathkey))
			{
				query_pathkeys_ok = false;
				break;
			}
		}

		if (query_pathkeys_ok)
		{
			useful_pathkeys_list = list_make1(list_copy(root->query_pathkeys));
			fpinfo->qp_is_pushdown_safe = true;
		}
	}

	/*
	 * Even if we're not using remote estimates, having the remote side do the
	 * sort generally won't be any worse than doing it locally, and it might
	 * be much better if the remote side can generate data in the right order
	 * without needing a sort at all.  However, what we're going to do next is
	 * try to generate pathkeys that seem promising for possible merge joins,
	 * and that's more speculative.  A wrong choice might hurt quite a bit, so
	 * bail out if we can't use remote estimates.
	 */
	if (!fpinfo->use_remote_estimate)
		return useful_pathkeys_list;

	/* Get the list of interesting EquivalenceClasses. */
	useful_eclass_list = get_useful_ecs_for_relation(root, rel);

	/* Extract unique EC for query, if any, so we don't consider it again. */
	if (list_length(root->query_pathkeys) == 1)
	{
		PathKey    *query_pathkey = linitial(root->query_pathkeys);

		query_ec = query_pathkey->pk_eclass;
	}

	/*
	 * As a heuristic, the only pathkeys we consider here are those of length
	 * one.  It's surely possible to consider more, but since each one we
	 * choose to consider will generate a round-trip to the remote side, we
	 * need to be a bit cautious here.  It would sure be nice to have a local
	 * cache of information about remote index definitions...
	 */
	foreach(lc, useful_eclass_list)
	{
		EquivalenceClass *cur_ec = lfirst(lc);
		PathKey    *pathkey;

		/* If redundant with what we did above, skip it. */
		if (cur_ec == query_ec)
			continue;

		/* Can't push down the sort if the EC's opfamily is not shippable. */
		if (!is_shippable(linitial_oid(cur_ec->ec_opfamilies),
						  OperatorFamilyRelationId, fpinfo))
			continue;

		/* If no pushable expression for this rel, skip it. */
		if (find_em_for_rel(root, cur_ec, rel) == NULL)
			continue;

		/* Looks like we can generate a pathkey, so let's do it. */
		pathkey = make_canonical_pathkey(root, cur_ec,
										 linitial_oid(cur_ec->ec_opfamilies),
										 BTLessStrategyNumber,
										 false);
		useful_pathkeys_list = lappend(useful_pathkeys_list,
									   list_make1(pathkey));
	}

	return useful_pathkeys_list;
}

/*
 * MonetDB_GetForeignPaths
 *		Create possible scan paths for a scan on the foreign table
 */
static void
MonetDB_GetForeignPaths(PlannerInfo *root,
						RelOptInfo *baserel,
						Oid foreigntableid)
{
	MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) baserel->fdw_private;
	ForeignPath *path;
	List	   *ppi_list;
	ListCell   *lc;

	/*
	 * Create simplest ForeignScan path node and add it to baserel.  This path
	 * corresponds to SeqScan path of regular tables (though depending on what
	 * baserestrict conditions we were able to send to remote, there might
	 * actually be an indexscan happening there).  We already did all the work
	 * to estimate cost and size of this path.
	 *
	 * Although this path uses no join clauses, it could still have required
	 * parameterization due to LATERAL refs in its tlist.
	 */
	path = create_foreignscan_path(root, baserel,
								   NULL,	/* default pathtarget */
								   fpinfo->rows,
								   fpinfo->startup_cost,
								   fpinfo->total_cost,
								   NIL, /* no pathkeys */
								   baserel->lateral_relids,
								   NULL,	/* no extra plan */
								   NIL);	/* no fdw_private list */
	add_path(baserel, (Path *) path);

	/* Add paths with pathkeys */
	add_paths_with_pathkeys_for_rel(root, baserel, NULL);

	/*
	 * If we're not using remote estimates, stop here.  We have no way to
	 * estimate whether any join clauses would be worth sending across, so
	 * don't bother building parameterized paths.
	 */
	if (!fpinfo->use_remote_estimate)
		return;

	/*
	 * Thumb through all join clauses for the rel to identify which outer
	 * relations could supply one or more safe-to-send-to-remote join clauses.
	 * We'll build a parameterized path for each such outer relation.
	 *
	 * It's convenient to manage this by representing each candidate outer
	 * relation by the ParamPathInfo node for it.  We can then use the
	 * ppi_clauses list in the ParamPathInfo node directly as a list of the
	 * interesting join clauses for that rel.  This takes care of the
	 * possibility that there are multiple safe join clauses for such a rel,
	 * and also ensures that we account for unsafe join clauses that we'll
	 * still have to enforce locally (since the parameterized-path machinery
	 * insists that we handle all movable clauses).
	 */
	ppi_list = NIL;
	foreach(lc, baserel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Relids		required_outer;
		ParamPathInfo *param_info;

		/* Check if clause can be moved to this rel */
		if (!join_clause_is_movable_to(rinfo, baserel))
			continue;

		/* See if it is safe to send to remote */
		if (!is_foreign_expr(root, baserel, rinfo->clause))
			continue;

		/* Calculate required outer rels for the resulting path */
		required_outer = bms_union(rinfo->clause_relids,
								   baserel->lateral_relids);
		/* We do not want the foreign rel itself listed in required_outer */
		required_outer = bms_del_member(required_outer, baserel->relid);

		/*
		 * required_outer probably can't be empty here, but if it were, we
		 * couldn't make a parameterized path.
		 */
		if (bms_is_empty(required_outer))
			continue;

		/* Get the ParamPathInfo */
		param_info = get_baserel_parampathinfo(root, baserel,
											   required_outer);
		Assert(param_info != NULL);

		/*
		 * Add it to list unless we already have it.  Testing pointer equality
		 * is OK since get_baserel_parampathinfo won't make duplicates.
		 */
		ppi_list = list_append_unique_ptr(ppi_list, param_info);
	}

	/*
	 * The above scan examined only "generic" join clauses, not those that
	 * were absorbed into EquivalenceClauses.  See if we can make anything out
	 * of EquivalenceClauses.
	 */
	if (baserel->has_eclass_joins)
	{
		/*
		 * We repeatedly scan the eclass list looking for column references
		 * (or expressions) belonging to the foreign rel.  Each time we find
		 * one, we generate a list of equivalence joinclauses for it, and then
		 * see if any are safe to send to the remote.  Repeat till there are
		 * no more candidate EC members.
		 */
		ec_member_foreign_arg arg;

		arg.already_used = NIL;
		for (;;)
		{
			List	   *clauses;

			/* Make clauses, skipping any that join to lateral_referencers */
			arg.current = NULL;
			clauses = generate_implied_equalities_for_column(root,
															 baserel,
															 ec_member_matches_foreign,
															 (void *) &arg,
															 baserel->lateral_referencers);

			/* Done if there are no more expressions in the foreign rel */
			if (arg.current == NULL)
			{
				Assert(clauses == NIL);
				break;
			}

			/* Scan the extracted join clauses */
			foreach(lc, clauses)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Relids		required_outer;
				ParamPathInfo *param_info;

				/* Check if clause can be moved to this rel */
				if (!join_clause_is_movable_to(rinfo, baserel))
					continue;

				/* See if it is safe to send to remote */
				if (!is_foreign_expr(root, baserel, rinfo->clause))
					continue;

				/* Calculate required outer rels for the resulting path */
				required_outer = bms_union(rinfo->clause_relids,
										   baserel->lateral_relids);
				required_outer = bms_del_member(required_outer, baserel->relid);
				if (bms_is_empty(required_outer))
					continue;

				/* Get the ParamPathInfo */
				param_info = get_baserel_parampathinfo(root, baserel,
													   required_outer);
				Assert(param_info != NULL);

				/* Add it to list unless we already have it */
				ppi_list = list_append_unique_ptr(ppi_list, param_info);
			}

			/* Try again, now ignoring the expression we found this time */
			arg.already_used = lappend(arg.already_used, arg.current);
		}
	}

	/*
	 * Now build a path for each useful outer relation.
	 */
	foreach(lc, ppi_list)
	{
		ParamPathInfo *param_info = (ParamPathInfo *) lfirst(lc);
		double		rows;
		int			width;
		Cost		startup_cost;
		Cost		total_cost;

		/* Get a cost estimate from the remote */
		estimate_path_cost_size(root, baserel,
								param_info->ppi_clauses, NIL, NULL,
								&rows, &width,
								&startup_cost, &total_cost);

		/*
		 * ppi_rows currently won't get looked at by anything, but still we
		 * may as well ensure that it matches our idea of the rowcount.
		 */
		param_info->ppi_rows = rows;

		/* Make the path */
		path = create_foreignscan_path(root, baserel,
									   NULL,	/* default pathtarget */
									   rows,
									   startup_cost,
									   total_cost,
									   NIL, /* no pathkeys */
									   param_info->ppi_req_outer,
									   NULL,
									   NIL);	/* no fdw_private list */
		add_path(baserel, (Path *) path);
	}
}

/*
 * MonetDB_GetForeignPlan
 *		Create ForeignScan plan node which implements selected best path
 */
static ForeignScan *
MonetDB_GetForeignPlan(PlannerInfo *root,
					   RelOptInfo *foreignrel,
					   Oid foreigntableid,
					   ForeignPath *best_path,
					   List *tlist,
					   List *scan_clauses,
					   Plan *outer_plan)
{
	MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) foreignrel->fdw_private;
	Index		scan_relid;
	List	   *fdw_private;
	List	   *remote_exprs = NIL;
	List	   *local_exprs = NIL;
	List	   *params_list = NIL;
	List	   *fdw_scan_tlist = NIL;
	List	   *fdw_recheck_quals = NIL;
	List	   *retrieved_attrs;
	StringInfoData sql;
	bool		has_final_sort = false;
	bool		has_limit = false;
	ListCell   *lc;

	/*
	 * Get FDW private data created by MonetDB_GetForeignUpperPaths(), if any.
	 */
	if (best_path->fdw_private)
	{
#if PG_VERSION_NUM >= 150000
		has_final_sort = boolVal(list_nth(best_path->fdw_private,
										  FdwPathPrivateHasFinalSort));
		has_limit = boolVal(list_nth(best_path->fdw_private,
									 FdwPathPrivateHasLimit));
#else
		has_final_sort = intVal(list_nth(best_path->fdw_private,
										 FdwPathPrivateHasFinalSort));
		has_limit = intVal(list_nth(best_path->fdw_private,
									FdwPathPrivateHasLimit));
#endif
	}

	if (IS_SIMPLE_REL(foreignrel))
	{
		/*
		 * For base relations, set scan_relid as the relid of the relation.
		 */
		scan_relid = foreignrel->relid;

		/*
		 * In a base-relation scan, we must apply the given scan_clauses.
		 *
		 * Separate the scan_clauses into those that can be executed remotely
		 * and those that can't.  baserestrictinfo clauses that were
		 * previously determined to be safe or unsafe by classifyConditions
		 * are found in fpinfo->remote_conds and fpinfo->local_conds. Anything
		 * else in the scan_clauses list will be a join clause, which we have
		 * to check for remote-safety.
		 *
		 * Note: the join clauses we see here should be the exact same ones
		 * previously examined by MonetDB_GetForeignPaths.  Possibly it'd be
		 * worth passing forward the classification work done then, rather
		 * than repeating it here.
		 *
		 * This code must match "extract_actual_clauses(scan_clauses, false)"
		 * except for the additional decision about remote versus local
		 * execution.
		 */
		foreach(lc, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			/* Ignore any pseudoconstants, they're dealt with elsewhere */
			if (rinfo->pseudoconstant)
				continue;

			if (list_member_ptr(fpinfo->remote_conds, rinfo))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else if (list_member_ptr(fpinfo->local_conds, rinfo))
				local_exprs = lappend(local_exprs, rinfo->clause);
			else if (is_foreign_expr(root, foreignrel, rinfo->clause))
				remote_exprs = lappend(remote_exprs, rinfo->clause);
			else
				local_exprs = lappend(local_exprs, rinfo->clause);
		}

		/*
		 * For a base-relation scan, we have to support EPQ recheck, which
		 * should recheck all the remote quals.
		 */
		fdw_recheck_quals = remote_exprs;
	}
	else
	{
		/*
		 * Join relation or upper relation - set scan_relid to 0.
		 */
		scan_relid = 0;

		/*
		 * For a join rel, baserestrictinfo is NIL and we are not considering
		 * parameterization right now, so there should be no scan_clauses for
		 * a joinrel or an upper rel either.
		 */
		Assert(!scan_clauses);

		/*
		 * Instead we get the conditions to apply from the fdw_private
		 * structure.
		 */
		remote_exprs = extract_actual_clauses(fpinfo->remote_conds, false);
		local_exprs = extract_actual_clauses(fpinfo->local_conds, false);

		/*
		 * We leave fdw_recheck_quals empty in this case, since we never need
		 * to apply EPQ recheck clauses.  In the case of a joinrel, EPQ
		 * recheck is handled elsewhere --- see MonetDB_GetForeignJoinPaths().
		 * If we're planning an upperrel (ie, remote grouping or aggregation)
		 * then there's no EPQ to do because SELECT FOR UPDATE wouldn't be
		 * allowed, and indeed we *can't* put the remote clauses into
		 * fdw_recheck_quals because the unaggregated Vars won't be available
		 * locally.
		 */

		/* Build the list of columns to be fetched from the foreign server. */
		fdw_scan_tlist = build_tlist_to_deparse(foreignrel);

		/*
		 * Ensure that the outer plan produces a tuple whose descriptor
		 * matches our scan tuple slot.  Also, remove the local conditions
		 * from outer plan's quals, lest they be evaluated twice, once by the
		 * local plan and once by the scan.
		 */
		if (outer_plan)
		{
			/*
			 * Right now, we only consider grouping and aggregation beyond
			 * joins. Queries involving aggregates or grouping do not require
			 * EPQ mechanism, hence should not have an outer plan here.
			 */
			Assert(!IS_UPPER_REL(foreignrel));

			/*
			 * First, update the plan's qual list if possible.  In some cases
			 * the quals might be enforced below the topmost plan level, in
			 * which case we'll fail to remove them; it's not worth working
			 * harder than this.
			 */
			foreach(lc, local_exprs)
			{
				Node	   *qual = lfirst(lc);

				outer_plan->qual = list_delete(outer_plan->qual, qual);

				/*
				 * For an inner join the local conditions of foreign scan plan
				 * can be part of the joinquals as well.  (They might also be
				 * in the mergequals or hashquals, but we can't touch those
				 * without breaking the plan.)
				 */
				if (IsA(outer_plan, NestLoop) ||
					IsA(outer_plan, MergeJoin) ||
					IsA(outer_plan, HashJoin))
				{
					Join	   *join_plan = (Join *) outer_plan;

					if (join_plan->jointype == JOIN_INNER)
						join_plan->joinqual = list_delete(join_plan->joinqual,
														  qual);
				}
			}

			/*
			 * Now fix the subplan's tlist --- this might result in inserting
			 * a Result node atop the plan tree.
			 */
			outer_plan = change_plan_targetlist(outer_plan, fdw_scan_tlist,
												best_path->path.parallel_safe);
		}
	}

	/*
	 * Build the query string to be sent for execution, and identify
	 * expressions to be sent as parameters.
	 */
	initStringInfo(&sql);
	deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
							remote_exprs, best_path->path.pathkeys,
							has_final_sort, has_limit, false,
							&retrieved_attrs, &params_list);

	/* Remember remote_exprs for possible use by PlanDirectModify */
	fpinfo->final_remote_exprs = remote_exprs;

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match order in enum FdwScanPrivateIndex.
	 */
	fdw_private = list_make3(makeString(sql.data),
							 retrieved_attrs,
							 makeInteger(fpinfo->fetch_size));
	if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name));

	/*
	 * Create the ForeignScan node for the given relation.
	 *
	 * Note that the remote parameter expressions are stored in the fdw_exprs
	 * field of the finished plan node; we can't keep them in private state
	 * because then they wouldn't be subject to later planner processing.
	 */
	return make_foreignscan(tlist,
							local_exprs,
							scan_relid,
							params_list,
							fdw_private,
							fdw_scan_tlist,
							fdw_recheck_quals,
							outer_plan);
}

/*
 * Construct a tuple descriptor for the scan tuples handled by a foreign join.
 */
static TupleDesc
get_tupdesc_for_join_scan_tuples(ForeignScanState *node)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	TupleDesc	tupdesc;

	/*
	 * The core code has already set up a scan tuple slot based on
	 * fsplan->fdw_scan_tlist, and this slot's tupdesc is mostly good enough,
	 * but there's one case where it isn't.  If we have any whole-row row
	 * identifier Vars, they may have vartype RECORD, and we need to replace
	 * that with the associated table's actual composite type.  This ensures
	 * that when we read those ROW() expression values from the remote server,
	 * we can convert them to a composite type the local server knows.
	 */
	tupdesc = CreateTupleDescCopy(node->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
	for (int i = 0; i < tupdesc->natts; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		Var		   *var;
		RangeTblEntry *rte;
		Oid			reltype;

		/* Nothing to do if it's not a generic RECORD attribute */
		if (att->atttypid != RECORDOID || att->atttypmod >= 0)
			continue;

		/*
		 * If we can't identify the referenced table, do nothing.  This'll
		 * likely lead to failure later, but perhaps we can muddle through.
		 */
		var = (Var *) list_nth_node(TargetEntry, fsplan->fdw_scan_tlist,
									i)->expr;
		if (!IsA(var, Var) || var->varattno != 0)
			continue;
		rte = list_nth(estate->es_range_table, var->varno - 1);
		if (rte->rtekind != RTE_RELATION)
			continue;
		reltype = get_rel_type_id(rte->relid);
		if (!OidIsValid(reltype))
			continue;
		att->atttypid = reltype;
		/* shouldn't need to change anything else */
	}
	return tupdesc;
}

/*
 * MonetDB_BeginForeignScan
 *		Initiate an executor scan of a foreign MonetDB_QL table.
 */
static void
MonetDB_BeginForeignScan(ForeignScanState *node, int eflags)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	EState	   *estate = node->ss.ps.state;
	MonetdbFdwScanState *fsstate;
	RangeTblEntry *rte;
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	int			rtindex;
	int			numParams;
	ForeignServer *server = NULL;

	/*
	 * We'll save private state in node->fdw_state.
	 */
	fsstate = (MonetdbFdwScanState *) palloc0(sizeof(MonetdbFdwScanState));
	node->fdw_state = (void *) fsstate;

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckPermissions() does.
	 */
	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
#if PG_VERSION_NUM >= 160000
	rtindex = bms_next_member(fsplan->fs_base_relids, -1);
#else
	rtindex = bms_next_member(fsplan->fs_relids, -1);
#endif

	rte = exec_rt_fetch(rtindex, estate);
#if PG_VERSION_NUM >= 160000
	userid = OidIsValid(fsplan->checkAsUser) ? fsplan->checkAsUser : GetUserId();
#else
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
#endif

	/* Get info about foreign table. */
	table = GetForeignTable(rte->relid);
	user = GetUserMapping(userid, table->serverid);
	server = GetForeignServer(table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	fsstate->conn = GetConnection(user, server);

	/* Get private info created by planner functions. */
	fsstate->query = strVal(list_nth(fsplan->fdw_private,
									 FdwScanPrivateSelectSql));
	fsstate->retrieved_attrs = (List *) list_nth(fsplan->fdw_private,
												 FdwScanPrivateRetrievedAttrs);
	fsstate->fetch_size = intVal(list_nth(fsplan->fdw_private,
										  FdwScanPrivateFetchSize));

	/* Create contexts for batches of tuples and per-tuple temp workspace. */
	fsstate->batch_cxt = AllocSetContextCreate(estate->es_query_cxt,
											   "monetdb_fdw tuple data",
											   ALLOCSET_DEFAULT_SIZES);
	fsstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "monetdb_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/*
	 * Get info we'll need for converting data fetched from the foreign server
	 * into local representation and error reporting during that process.
	 */
	if (fsplan->scan.scanrelid > 0)
	{
		fsstate->rel = node->ss.ss_currentRelation;
		fsstate->tupdesc = RelationGetDescr(fsstate->rel);
	}
	else
	{
		fsstate->rel = NULL;
		fsstate->tupdesc = get_tupdesc_for_join_scan_tuples(node);
	}

	fsstate->attinmeta = TupleDescGetAttInMetadata(fsstate->tupdesc);

	/*
	 * Prepare for processing of parameters used in remote query, if any.
	 */
	numParams = list_length(fsplan->fdw_exprs);
	fsstate->numParams = numParams;
	if (numParams > 0)
		prepare_query_params((PlanState *) node,
							 fsplan->fdw_exprs,
							 numParams,
							 &fsstate->param_flinfo,
							 &fsstate->param_exprs,
							 &fsstate->param_values);

	/* Set the async-capable flag */
	fsstate->async_capable = node->ss.ps.async_capable;
}

/*
 * MonetDB_IterateForeignScan
 *		Retrieve next row from the result set, or clear tuple slot to indicate
 *		EOF.
 */
static TupleTableSlot *
MonetDB_IterateForeignScan(ForeignScanState *node)
{
	MonetdbFdwScanState *fsstate = (MonetdbFdwScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;

	if (!fsstate->hdl)
	{
		elog(DEBUG2, "monetdb_fdw remote query is: %s", fsstate->query);
		/* Submit a query and wait for the result. */
		if ((fsstate->hdl = mapi_query(fsstate->conn, fsstate->query)) == NULL ||
			mapi_error(fsstate->conn) != MOK)
		{
			if (mapi_error(fsstate->conn))
				die(fsstate->conn, fsstate->hdl);
			if (mapi_close_handle(fsstate->hdl) != MOK)
				die(fsstate->conn, fsstate->hdl);
		}
	}

	/*
	 * Get some more tuples.
	 */
	if (fsstate->next_tuple >= fsstate->num_tuples)
	{
		/* In async mode, just clear tuple slot. */
		if (fsstate->async_capable)
			return ExecClearTuple(slot);
		/* No point in another fetch if we already detected EOF, though. */
		if (!fsstate->eof_reached)
			fetch_more_data(node);
		/* If we didn't get any tuples, must be end of data. */
		if (fsstate->next_tuple >= fsstate->num_tuples)
			return ExecClearTuple(slot);
	}

	/*
	 * Return the next tuple.
	 */
	ExecStoreHeapTuple(fsstate->tuples[fsstate->next_tuple++],
					   slot,
					   false);

	return slot;
}

/*
 * MonetDB_ReScanForeignScan
 *		Restart the scan.
 */
static void
MonetDB_ReScanForeignScan(ForeignScanState *node)
{
	elog(ERROR, "MonetDB_ReScanForeignScan not supported yet");
}

/*
 * MonetDB_EndForeignScan
 *		Finish scanning foreign table and dispose objects used for this scan
 */
static void
MonetDB_EndForeignScan(ForeignScanState *node)
{
	MonetdbFdwScanState *fsstate = (MonetdbFdwScanState *) node->fdw_state;

	/* if fsstate is NULL, we are in EXPLAIN; nothing to do */
	if (fsstate == NULL)
		return;

	/* Release remote connection */
	ReleaseConnection(fsstate->conn);
	fsstate->conn = NULL;

	/* MemoryContexts will be deleted automatically. */
}

/*
 * MonetDB_AddForeignUpdateTargets
 *		Add resjunk column(s) needed for update/delete on a foreign table
 */
static void
MonetDB_AddForeignUpdateTargets(PlannerInfo *root,
								Index rtindex,
								RangeTblEntry *target_rte,
								Relation target_relation)
{
	bool 	  has_key = false;
	Oid 	  relid   = RelationGetRelid(target_relation);
	TupleDesc tupdesc = target_relation->rd_att;

	/* loop through all columns of the foreign table */
	for (int i = 0; i < tupdesc->natts; ++i)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, i);
		ListCell 	*option;

		/* look for the "key" option on this column */
		List 	*options = GetForeignColumnOptions(relid, att->attnum);
		foreach(option, options)
		{
			DefElem *def = (DefElem *)lfirst(option);

			/* if "key" is set, add a resjunk for this column */
			if (strcmp(def->defname, OPT_KEY) == 0)
			{
				if (getBoolVal(def))
				{
					Var *var = makeVar(rtindex,
										att->attnum,
										att->atttypid,
										att->atttypmod,
										att->attcollation,
										0);
					add_row_identity_var(root, var, rtindex, NameStr(att->attname));
					has_key = true;
				}
			}
		}
	}

	if (!has_key)
		ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
				errmsg("no primary key column specified for foreign MonetDB table"),
				errdetail("For UPDATE or DELETE, at least one foreign table column must be marked as primary key column.")));
}

/*
 * MonetDB_PlanForeignModify
 *		Plan an insert/update/delete operation on a foreign table
 */
static List *
MonetDB_PlanForeignModify(PlannerInfo *root,
						  ModifyTable *plan,
						  Index resultRelation,
						  int subplan_index)
{
	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	StringInfoData sql;
	List	   *targetAttrs = NIL;
	List	   *withCheckOptionList = NIL;
	List	   *returningList = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;
	int			values_end_len = -1;

	initStringInfo(&sql);

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = table_open(rte->relid, NoLock);

	/*
	 * In an INSERT, we transmit all columns that are defined in the foreign
	 * table.  In an UPDATE, if there are BEFORE ROW UPDATE triggers on the
	 * foreign table, we transmit all columns like INSERT; else we transmit
	 * only columns that were explicitly targets of the UPDATE, so as to avoid
	 * unnecessary data transmission.  (We can't do that for INSERT since we
	 * would miss sending default values for columns not listed in the source
	 * statement, and for UPDATE if there are BEFORE ROW UPDATE triggers since
	 * those triggers might change values for non-target columns, in which
	 * case we would miss sending changed values for those columns.)
	 */
	if (operation == CMD_INSERT ||
		(operation == CMD_UPDATE &&
		 rel->trigdesc &&
		 rel->trigdesc->trig_update_before_row))
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		int			col;
		RelOptInfo *rel = find_base_rel(root, resultRelation);
		Bitmapset  *allUpdatedCols = get_rel_all_updated_cols(root, rel);

		col = -1;
		while ((col = bms_next_member(allUpdatedCols, col)) >= 0)
		{
			/* bit numbers are offset by FirstLowInvalidHeapAttributeNumber */
			AttrNumber	attno = col + FirstLowInvalidHeapAttributeNumber;

			if (attno <= InvalidAttrNumber) /* shouldn't happen */
				elog(ERROR, "system-column update is not supported");
			targetAttrs = lappend_int(targetAttrs, attno);
		}
	}

	/*
	 * Extract the relevant WITH CHECK OPTION list if any.
	 */
	if (plan->withCheckOptionLists)
		withCheckOptionList = (List *) list_nth(plan->withCheckOptionLists,
												subplan_index);

	/*
	 * Extract the relevant RETURNING list if any.
	 */
	if (plan->returningLists)
		returningList = (List *) list_nth(plan->returningLists, subplan_index);

	/*
	 * ON CONFLICT DO UPDATE and DO NOTHING case with inference specification
	 * should have already been rejected in the optimizer, as presently there
	 * is no way to recognize an arbiter index on a foreign table.  Only DO
	 * NOTHING is supported without an inference specification.
	 */
	if (plan->onConflictAction == ONCONFLICT_NOTHING)
		doNothing = true;
	else if (plan->onConflictAction != ONCONFLICT_NONE)
		elog(ERROR, "unexpected ON CONFLICT specification: %d",
			 (int) plan->onConflictAction);

	/*
	 * Construct the SQL command string.
	 */
	switch (operation)
	{
		case CMD_INSERT:
			deparseInsertSql(&sql, rte, resultRelation, rel,
							 targetAttrs, doNothing,
							 withCheckOptionList, returningList,
							 &retrieved_attrs, &values_end_len);
			break;
		case CMD_UPDATE:
			deparseUpdateSql(&sql, rte, resultRelation, rel,
							 targetAttrs,
							 withCheckOptionList, returningList,
							 &retrieved_attrs);
			break;
		case CMD_DELETE:
			deparseDeleteSql(&sql, rte, resultRelation, rel,
							 returningList,
							 &retrieved_attrs);
			break;
		default:
			elog(ERROR, "unexpected operation: %d", (int) operation);
			break;
	}

	table_close(rel, NoLock);

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum FdwModifyPrivateIndex, above.
	 */
#if PG_VERSION_NUM >= 150000
	return list_make5(makeString(sql.data),
					  targetAttrs,
					  makeInteger(values_end_len),
					  makeBoolean((retrieved_attrs != NIL)),
					  retrieved_attrs);
#else
	return list_make5(makeString(sql.data),
						targetAttrs,
						makeInteger(values_end_len),
						makeInteger((retrieved_attrs != NIL)),
						retrieved_attrs);
#endif
}

/*
 * MonetDB_BeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table
 */
static void
MonetDB_BeginForeignModify(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo,
						   List *fdw_private,
						   int subplan_index,
						   int eflags)
{
	MonetdbFdwModifyState *fmstate;
	char	   *query;
	List	   *target_attrs;
	bool		has_returning;
	int			values_end_len;
	List	   *retrieved_attrs;
	RangeTblEntry *rte;

	/* Deconstruct fdw_private data. */
	query = strVal(list_nth(fdw_private,
							FdwModifyPrivateUpdateSql));
	target_attrs = (List *) list_nth(fdw_private,
									 FdwModifyPrivateTargetAttnums);
	values_end_len = intVal(list_nth(fdw_private,
									 FdwModifyPrivateLen));
#if PG_VERSION_NUM >= 150000	
	has_returning = boolVal(list_nth(fdw_private,
									 FdwModifyPrivateHasReturning));
#else
	has_returning = intVal(list_nth(fdw_private,
									 FdwModifyPrivateHasReturning));
#endif
	retrieved_attrs = (List *) list_nth(fdw_private,
										FdwModifyPrivateRetrievedAttrs);

	/* Find RTE. */
	rte = exec_rt_fetch(resultRelInfo->ri_RangeTableIndex,
						mtstate->ps.state);

	/* Construct an execution state. */
	fmstate = create_foreign_modify(mtstate->ps.state,
									rte,
									resultRelInfo,
									mtstate->operation,
									outerPlanState(mtstate)->plan,
									query,
									target_attrs,
									values_end_len,
									has_returning,
									retrieved_attrs);

	resultRelInfo->ri_FdwState = fmstate;
}

/*
 * MonetDB_ExecForeignInsert
 *		Insert one row into a foreign table
 */
static TupleTableSlot *
MonetDB_ExecForeignInsert(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	MonetdbFdwModifyState *fmstate = (MonetdbFdwModifyState *) resultRelInfo->ri_FdwState;
	TupleTableSlot **rslot;
	int			numSlots = 1;

	/*
	 * If the fmstate has aux_fmstate set, use the aux_fmstate (see
	 * MonetDB_BeginForeignInsert())
	 */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate->aux_fmstate;
	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_INSERT,
								   &slot, &planSlot, &numSlots);
	/* Revert that change */
	if (fmstate->aux_fmstate)
		resultRelInfo->ri_FdwState = fmstate;

	return rslot ? *rslot : NULL;
}

/*
 * MonetDB_ExecForeignBatchInsert
 *		Insert multiple rows into a foreign table
 */
static TupleTableSlot **
MonetDB_ExecForeignBatchInsert(EState *estate,
							   ResultRelInfo *resultRelInfo,
							   TupleTableSlot **slots,
							   TupleTableSlot **planSlots,
							   int *numSlots)
{
	elog(ERROR, "MonetDB_ExecForeignBatchInsert not supported yet");
}

/*
 * MonetDB_ExecForeignUpdate
 *		Update one row in a foreign table
 */
static TupleTableSlot *
MonetDB_ExecForeignUpdate(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	TupleTableSlot **rslot;
	int			numSlots = 1;

	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_UPDATE,
								   &slot, &planSlot, &numSlots);

	return rslot ? rslot[0] : NULL;
}

/*
 * MonetDB_ExecForeignDelete
 *		Delete one row from a foreign table
 */
static TupleTableSlot *
MonetDB_ExecForeignDelete(EState *estate,
						  ResultRelInfo *resultRelInfo,
						  TupleTableSlot *slot,
						  TupleTableSlot *planSlot)
{
	TupleTableSlot **rslot;
	int			numSlots = 1;

	rslot = execute_foreign_modify(estate, resultRelInfo, CMD_DELETE,
								   &slot, &planSlot, &numSlots);

	return rslot ? rslot[0] : NULL;
}

/*
 * MonetDB_EndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
MonetDB_EndForeignModify(EState *estate,
						 ResultRelInfo *resultRelInfo)
{
	MonetdbFdwModifyState *fmstate = (MonetdbFdwModifyState *) resultRelInfo->ri_FdwState;

	/* If fmstate is NULL, we are in EXPLAIN; nothing to do */
	if (fmstate == NULL)
		return;

	/* Release remote connection */
	ReleaseConnection(fmstate->conn);
	fmstate->conn = NULL;
}

/*
 * MonetDB_BeginForeignInsert
 *		Begin an insert operation on a foreign table
 */
static void
MonetDB_BeginForeignInsert(ModifyTableState *mtstate,
						   ResultRelInfo *resultRelInfo)
{
	MonetdbFdwModifyState *fmstate;
	ModifyTable *plan = castNode(ModifyTable, mtstate->ps.plan);
	EState	   *estate = mtstate->ps.state;
	Index		resultRelation;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	RangeTblEntry *rte;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			attnum;
	int			values_end_len;
	StringInfoData sql;
	List	   *targetAttrs = NIL;
	List	   *retrieved_attrs = NIL;
	bool		doNothing = false;

	/*
	 * If the foreign table we are about to insert routed rows into is also an
	 * UPDATE subplan result rel that will be updated later, proceeding with
	 * the INSERT will result in the later UPDATE incorrectly modifying those
	 * routed rows, so prevent the INSERT --- it would be nice if we could
	 * handle this case; but for now, throw an error for safety.
	 */
	if (plan && plan->operation == CMD_UPDATE &&
		(resultRelInfo->ri_usesFdwDirectModify ||
		 resultRelInfo->ri_FdwState))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot route tuples into foreign table to be updated \"%s\"",
						RelationGetRelationName(rel))));

	initStringInfo(&sql);

	/* We transmit all columns that are defined in the foreign table. */
	for (attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

		if (!attr->attisdropped)
			targetAttrs = lappend_int(targetAttrs, attnum);
	}

	/* Check if we add the ON CONFLICT clause to the remote query. */
	if (plan)
	{
		OnConflictAction onConflictAction = plan->onConflictAction;

		/* We only support DO NOTHING without an inference specification. */
		if (onConflictAction == ONCONFLICT_NOTHING)
			doNothing = true;
		else if (onConflictAction != ONCONFLICT_NONE)
			elog(ERROR, "unexpected ON CONFLICT specification: %d",
				 (int) onConflictAction);
	}

	/*
	 * If the foreign table is a partition that doesn't have a corresponding
	 * RTE entry, we need to create a new RTE describing the foreign table for
	 * use by deparseInsertSql and create_foreign_modify() below, after first
	 * copying the parent's RTE and modifying some fields to describe the
	 * foreign partition to work on. However, if this is invoked by UPDATE,
	 * the existing RTE may already correspond to this partition if it is one
	 * of the UPDATE subplan target rels; in that case, we can just use the
	 * existing RTE as-is.
	 */
	if (resultRelInfo->ri_RangeTableIndex == 0)
	{
		ResultRelInfo *rootResultRelInfo = resultRelInfo->ri_RootResultRelInfo;

		rte = exec_rt_fetch(rootResultRelInfo->ri_RangeTableIndex, estate);
		rte = copyObject(rte);
		rte->relid = RelationGetRelid(rel);
		rte->relkind = RELKIND_FOREIGN_TABLE;

		/*
		 * For UPDATE, we must use the RT index of the first subplan target
		 * rel's RTE, because the core code would have built expressions for
		 * the partition, such as RETURNING, using that RT index as varno of
		 * Vars contained in those expressions.
		 */
		if (plan && plan->operation == CMD_UPDATE &&
			rootResultRelInfo->ri_RangeTableIndex == plan->rootRelation)
			resultRelation = mtstate->resultRelInfo[0].ri_RangeTableIndex;
		else
			resultRelation = rootResultRelInfo->ri_RangeTableIndex;
	}
	else
	{
		resultRelation = resultRelInfo->ri_RangeTableIndex;
		rte = exec_rt_fetch(resultRelation, estate);
	}

	/* Construct the SQL command string. */
	deparseInsertSql(&sql, rte, resultRelation, rel, targetAttrs, doNothing,
					 resultRelInfo->ri_WithCheckOptions,
					 resultRelInfo->ri_returningList,
					 &retrieved_attrs, &values_end_len);

	/* Construct an execution state. */
	fmstate = create_foreign_modify(mtstate->ps.state,
									rte,
									resultRelInfo,
									CMD_INSERT,
									NULL,
									sql.data,
									targetAttrs,
									values_end_len,
									retrieved_attrs != NIL,
									retrieved_attrs);

	fmstate->query = pstrdup(sql.data);
	/*
	 * If the given resultRelInfo already has MonetdbFdwModifyState set, it means
	 * the foreign table is an UPDATE subplan result rel; in which case, store
	 * the resulting state into the aux_fmstate of the MonetdbFdwModifyState.
	 */
	if (resultRelInfo->ri_FdwState)
	{
		Assert(plan && plan->operation == CMD_UPDATE);
		Assert(resultRelInfo->ri_usesFdwDirectModify == false);
		((MonetdbFdwModifyState *) resultRelInfo->ri_FdwState)->aux_fmstate = fmstate;
	}
	else
		resultRelInfo->ri_FdwState = fmstate;
}

/*
 * MonetDB_EndForeignInsert
 *		Finish an insert operation on a foreign table
 */
static void
MonetDB_EndForeignInsert(EState *estate,
						 ResultRelInfo *resultRelInfo)
{
	elog(ERROR, "MonetDB_EndForeignInsert not supported yet");
}

/*
 * MonetDB_IsForeignRelUpdatable
 *		Determine whether a foreign table supports INSERT, UPDATE and/or
 *		DELETE.
 */
static int
MonetDB_IsForeignRelUpdatable(Relation rel)
{
	bool		updatable;
	ForeignTable *table;
	ForeignServer *server;
	ListCell   *lc;

	/*
	 * By default, all monetdb_fdw foreign tables are assumed updatable. This
	 * can be overridden by a per-server setting, which in turn can be
	 * overridden by a per-table setting.
	 */
	updatable = true;

	table = GetForeignTable(RelationGetRelid(rel));
	server = GetForeignServer(table->serverid);

	foreach(lc, server->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "updatable") == 0)
			updatable = defGetBoolean(def);
	}
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "updatable") == 0)
			updatable = defGetBoolean(def);
	}

	/*
	 * Currently "updatable" means support for INSERT, UPDATE and DELETE.
	 */
	return updatable ?
		(1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE) : 0;
}

/*
 * MonetDB_RecheckForeignScan
 *		Execute a local join execution plan for a foreign join
 */
static bool
MonetDB_RecheckForeignScan(ForeignScanState *node, TupleTableSlot *slot)
{
	elog(ERROR, "MonetDB_RecheckForeignScan not supported yet");
}

/*
 * MonetDB_BeginDirectModify
 *		Prepare a direct foreign table modification
 */
static void
MonetDB_BeginDirectModify(ForeignScanState *node, int eflags)
{
	elog(ERROR, "MonetDB_BeginDirectModify not supported yet");
}

/*
 * MonetDB_IterateDirectModify
 *		Execute a direct foreign table modification
 */
static TupleTableSlot *
MonetDB_IterateDirectModify(ForeignScanState *node)
{
	elog(ERROR, "MonetDB_IterateDirectModify not supported yet");
}

/*
 * MonetDB_EndDirectModify
 *		Finish a direct foreign table modification
 */
static void
MonetDB_EndDirectModify(ForeignScanState *node)
{
	elog(ERROR, "MonetDB_EndDirectModify not supported yet");
}

/*
 * MonetDB_ExplainForeignScan
 *		Produce extra output for EXPLAIN of a ForeignScan on a foreign table
 */
static void
MonetDB_ExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	MonetdbFdwScanState *fsstate = (MonetdbFdwScanState *) node->fdw_state;
	char	*remote_sql = psprintf("plan %s", fsstate->query);
	elog(DEBUG2, "monetdb_fdw remote query is: %s", remote_sql);

	/* show query */
	ExplainPropertyText("MonetDB query", fsstate->query, es);
	if ((fsstate->hdl = mapi_query(fsstate->conn, remote_sql)) == NULL || mapi_error(fsstate->conn))
		die(fsstate->conn, fsstate->hdl);
	/* show plan */
	while (mapi_fetch_row(fsstate->hdl)) {
		ExplainPropertyText("MonetDB plan", mapi_fetch_field(fsstate->hdl, 0), es);
	}
	pfree(remote_sql);
}

/*
 * MonetDB_ExplainForeignModify
 *		Produce extra output for EXPLAIN of a ModifyTable on a foreign table
 */
static void
MonetDB_ExplainForeignModify(ModifyTableState *mtstate,
							 ResultRelInfo *rinfo,
							 List *fdw_private,
							 int subplan_index,
							 ExplainState *es)
{
	MonetdbFdwModifyState *fsstate = (MonetdbFdwModifyState *) rinfo->ri_FdwState;

	/* show query */
	ExplainPropertyText("MonetDB statement", fsstate->query, es);
}

/*
 * MonetDB_ExplainDirectModify
 *		Produce extra output for EXPLAIN of a ForeignScan that modifies a
 *		foreign table directly
 */
static void
MonetDB_ExplainDirectModify(ForeignScanState *node, ExplainState *es)
{
	elog(ERROR, "MonetDB_ExplainDirectModify not supported yet");
}

/*
 * MonetDB_ExecForeignTruncate
 *		Truncate one or more foreign tables
 */
static void
MonetDB_ExecForeignTruncate(List *rels,
							DropBehavior behavior,
							bool restart_seqs)
{
	Oid			serverid = InvalidOid;
	StringInfoData sql;
	ListCell   *lc;
	bool		server_truncatable = true;
	Mapi 	   	conn;
	MapiHdl 	hdl;
	UserMapping *user = NULL;
	ForeignServer *fserver = NULL;

	/*
	 * By default, all monetdb_fdw foreign tables are assumed truncatable.
	 * This can be overridden by a per-server setting, which in turn can be
	 * overridden by a per-table setting.
	 */
	foreach(lc, rels)
	{
		ForeignServer *server = NULL;
		Relation	rel = lfirst(lc);
		ForeignTable *table = GetForeignTable(RelationGetRelid(rel));
		ListCell   *cell;
		bool		truncatable;

		/*
		 * First time through, determine whether the foreign server allows
		 * truncates. Since all specified foreign tables are assumed to belong
		 * to the same foreign server, this result can be used for other
		 * foreign tables.
		 */
		if (!OidIsValid(serverid))
		{
			serverid = table->serverid;
			server = GetForeignServer(serverid);

			foreach(cell, server->options)
			{
				DefElem    *defel = (DefElem *) lfirst(cell);

				if (strcmp(defel->defname, "truncatable") == 0)
				{
					server_truncatable = defGetBoolean(defel);
					break;
				}
			}
		}

		/*
		 * Confirm that all specified foreign tables belong to the same
		 * foreign server.
		 */
		Assert(table->serverid == serverid);

		/* Determine whether this foreign table allows truncations */
		truncatable = server_truncatable;
		foreach(cell, table->options)
		{
			DefElem    *defel = (DefElem *) lfirst(cell);

			if (strcmp(defel->defname, "truncatable") == 0)
			{
				truncatable = defGetBoolean(defel);
				break;
			}
		}

		if (!truncatable)
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("foreign table \"%s\" does not allow truncates",
							RelationGetRelationName(rel))));
	}
	Assert(OidIsValid(serverid));

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	user = GetUserMapping(GetUserId(), serverid);
	fserver = GetForeignServer(serverid);

	/* Construct the TRUNCATE command string */
	initStringInfo(&sql);
	deparseTruncateSql(&sql, rels, behavior, restart_seqs);
	elog(DEBUG2, "monetdb_fdw remote query is: %s", sql.data);
	
	/* Issue the TRUNCATE command to remote server */
	conn = GetConnection(user, fserver);
	if ((hdl = mapi_query(conn, sql.data)) == NULL || mapi_error(conn))
		die(conn, hdl);

	pfree(sql.data);
}

/*
 * estimate_path_cost_size
 *		Get cost and size estimates for a foreign scan on given foreign relation
 *		either a base relation or a join between foreign relations or an upper
 *		relation containing foreign relations.
 *
 * param_join_conds are the parameterization clauses with outer relations.
 * pathkeys specify the expected sort order if any for given path being costed.
 * fpextra specifies additional post-scan/join-processing steps such as the
 * final sort and the LIMIT restriction.
 *
 * The function returns the cost and size estimates in p_rows, p_width,
 * p_startup_cost and p_total_cost variables.
 */
static void
estimate_path_cost_size(PlannerInfo *root,
						RelOptInfo *foreignrel,
						List *param_join_conds,
						List *pathkeys,
						MonetdbFdwPathExtraData *fpextra,
						double *p_rows, int *p_width,
						Cost *p_startup_cost, Cost *p_total_cost)
{
	MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) foreignrel->fdw_private;
	double		rows;
	double		retrieved_rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/* Make sure the core code has set up the relation's reltarget */
	Assert(foreignrel->reltarget);

	/*
	 * If the table or the server is configured to use remote estimates,
	 * connect to the foreign server and execute EXPLAIN to estimate the
	 * number of rows selected by the restriction+join clauses.  Otherwise,
	 * estimate rows using whatever statistics we have locally, in a way
	 * similar to ordinary tables.
	 */
	if (fpinfo->use_remote_estimate)
	{
		List	   *remote_param_join_conds;
		List	   *local_param_join_conds;
		StringInfoData sql;
		Selectivity local_sel;
		QualCost	local_cost;
		List	   *fdw_scan_tlist = NIL;
		List	   *remote_conds;

		/* Required only to be passed to deparseSelectStmtForRel */
		List	   *retrieved_attrs;

		/*
		 * param_join_conds might contain both clauses that are safe to send
		 * across, and clauses that aren't.
		 */
		classifyConditions(root, foreignrel, param_join_conds,
						   &remote_param_join_conds, &local_param_join_conds);

		/* Build the list of columns to be fetched from the foreign server. */
		if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
			fdw_scan_tlist = build_tlist_to_deparse(foreignrel);
		else
			fdw_scan_tlist = NIL;

		/*
		 * The complete list of remote conditions includes everything from
		 * baserestrictinfo plus any extra join_conds relevant to this
		 * particular path.
		 */
		remote_conds = list_concat(remote_param_join_conds,
								   fpinfo->remote_conds);

		/*
		 * Construct EXPLAIN query including the desired SELECT, FROM, and
		 * WHERE clauses. Params and other-relation Vars are replaced by dummy
		 * values, so don't request params_list.
		 */
		initStringInfo(&sql);
		appendStringInfoString(&sql, "EXPLAIN ");
		deparseSelectStmtForRel(&sql, root, foreignrel, fdw_scan_tlist,
								remote_conds, pathkeys,
								fpextra ? fpextra->has_final_sort : false,
								fpextra ? fpextra->has_limit : false,
								false, &retrieved_attrs, NULL);

		retrieved_rows = rows;

		/* Factor in the selectivity of the locally-checked quals */
		local_sel = clauselist_selectivity(root,
										   local_param_join_conds,
										   foreignrel->relid,
										   JOIN_INNER,
										   NULL);
		local_sel *= fpinfo->local_conds_sel;

		rows = clamp_row_est(rows * local_sel);

		/* Add in the eval cost of the locally-checked quals */
		startup_cost += fpinfo->local_conds_cost.startup;
		total_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
		cost_qual_eval(&local_cost, local_param_join_conds, root);
		startup_cost += local_cost.startup;
		total_cost += local_cost.per_tuple * retrieved_rows;

		/*
		 * Add in tlist eval cost for each output row.  In case of an
		 * aggregate, some of the tlist expressions such as grouping
		 * expressions will be evaluated remotely, so adjust the costs.
		 */
		startup_cost += foreignrel->reltarget->cost.startup;
		total_cost += foreignrel->reltarget->cost.startup;
		total_cost += foreignrel->reltarget->cost.per_tuple * rows;
		if (IS_UPPER_REL(foreignrel))
		{
			QualCost	tlist_cost;

			cost_qual_eval(&tlist_cost, fdw_scan_tlist, root);
			startup_cost -= tlist_cost.startup;
			total_cost -= tlist_cost.startup;
			total_cost -= tlist_cost.per_tuple * rows;
		}
	}
	else
	{
		Cost		run_cost = 0;

		/*
		 * We don't support join conditions in this mode (hence, no
		 * parameterized paths can be made).
		 */
		Assert(param_join_conds == NIL);

		/*
		 * We will come here again and again with different set of pathkeys or
		 * additional post-scan/join-processing steps that caller wants to
		 * cost.  We don't need to calculate the cost/size estimates for the
		 * underlying scan, join, or grouping each time.  Instead, use those
		 * estimates if we have cached them already.
		 */
		if (fpinfo->rel_startup_cost >= 0 && fpinfo->rel_total_cost >= 0)
		{
			Assert(fpinfo->retrieved_rows >= 0);

			rows = fpinfo->rows;
			retrieved_rows = fpinfo->retrieved_rows;
			width = fpinfo->width;
			startup_cost = fpinfo->rel_startup_cost;
			run_cost = fpinfo->rel_total_cost - fpinfo->rel_startup_cost;

			/*
			 * If we estimate the costs of a foreign scan or a foreign join
			 * with additional post-scan/join-processing steps, the scan or
			 * join costs obtained from the cache wouldn't yet contain the
			 * eval costs for the final scan/join target, which would've been
			 * updated by apply_scanjoin_target_to_paths(); add the eval costs
			 * now.
			 */
			if (fpextra && !IS_UPPER_REL(foreignrel))
			{
				/* Shouldn't get here unless we have LIMIT */
				Assert(fpextra->has_limit);
				Assert(foreignrel->reloptkind == RELOPT_BASEREL ||
					   foreignrel->reloptkind == RELOPT_JOINREL);
				startup_cost += foreignrel->reltarget->cost.startup;
				run_cost += foreignrel->reltarget->cost.per_tuple * rows;
			}
		}
		else if (IS_JOIN_REL(foreignrel))
		{
			MonetdbFdwRelationInfo *fpinfo_i;
			MonetdbFdwRelationInfo *fpinfo_o;
			QualCost	join_cost;
			QualCost	remote_conds_cost;
			double		nrows;

			/* Use rows/width estimates made by the core code. */
			rows = foreignrel->rows;
			width = foreignrel->reltarget->width;

			/* For join we expect inner and outer relations set */
			Assert(fpinfo->innerrel && fpinfo->outerrel);

			fpinfo_i = (MonetdbFdwRelationInfo *) fpinfo->innerrel->fdw_private;
			fpinfo_o = (MonetdbFdwRelationInfo *) fpinfo->outerrel->fdw_private;

			/* Estimate of number of rows in cross product */
			nrows = fpinfo_i->rows * fpinfo_o->rows;

			/*
			 * Back into an estimate of the number of retrieved rows.  Just in
			 * case this is nuts, clamp to at most nrows.
			 */
			retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
			retrieved_rows = Min(retrieved_rows, nrows);

			/*
			 * The cost of foreign join is estimated as cost of generating
			 * rows for the joining relations + cost for applying quals on the
			 * rows.
			 */

			/*
			 * Calculate the cost of clauses pushed down to the foreign server
			 */
			cost_qual_eval(&remote_conds_cost, fpinfo->remote_conds, root);
			/* Calculate the cost of applying join clauses */
			cost_qual_eval(&join_cost, fpinfo->joinclauses, root);

			/*
			 * Startup cost includes startup cost of joining relations and the
			 * startup cost for join and other clauses. We do not include the
			 * startup cost specific to join strategy (e.g. setting up hash
			 * tables) since we do not know what strategy the foreign server
			 * is going to use.
			 */
			startup_cost = fpinfo_i->rel_startup_cost + fpinfo_o->rel_startup_cost;
			startup_cost += join_cost.startup;
			startup_cost += remote_conds_cost.startup;
			startup_cost += fpinfo->local_conds_cost.startup;

			/*
			 * Run time cost includes:
			 *
			 * 1. Run time cost (total_cost - startup_cost) of relations being
			 * joined
			 *
			 * 2. Run time cost of applying join clauses on the cross product
			 * of the joining relations.
			 *
			 * 3. Run time cost of applying pushed down other clauses on the
			 * result of join
			 *
			 * 4. Run time cost of applying nonpushable other clauses locally
			 * on the result fetched from the foreign server.
			 */
			run_cost = fpinfo_i->rel_total_cost - fpinfo_i->rel_startup_cost;
			run_cost += fpinfo_o->rel_total_cost - fpinfo_o->rel_startup_cost;
			run_cost += nrows * join_cost.per_tuple;
			nrows = clamp_row_est(nrows * fpinfo->joinclause_sel);
			run_cost += nrows * remote_conds_cost.per_tuple;
			run_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;

			/* Add in tlist eval cost for each output row */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}
		else if (IS_UPPER_REL(foreignrel))
		{
			RelOptInfo *outerrel = fpinfo->outerrel;
			MonetdbFdwRelationInfo *ofpinfo;
			AggClauseCosts aggcosts;
			double		input_rows;
			int			numGroupCols;
			double		numGroups = 1;

			/* The upper relation should have its outer relation set */
			Assert(outerrel);
			/* and that outer relation should have its reltarget set */
			Assert(outerrel->reltarget);

			/*
			 * This cost model is mixture of costing done for sorted and
			 * hashed aggregates in cost_agg().  We are not sure which
			 * strategy will be considered at remote side, thus for
			 * simplicity, we put all startup related costs in startup_cost
			 * and all finalization and run cost are added in total_cost.
			 */

			ofpinfo = (MonetdbFdwRelationInfo *) outerrel->fdw_private;

			/* Get rows from input rel */
			input_rows = ofpinfo->rows;

			/* Collect statistics about aggregates for estimating costs. */
			MemSet(&aggcosts, 0, sizeof(AggClauseCosts));
			if (root->parse->hasAggs)
			{
				get_agg_clause_costs(root, AGGSPLIT_SIMPLE, &aggcosts);
			}

			/* Get number of grouping columns and possible number of groups */
#if PG_VERSION_NUM >= 160000
			numGroupCols = list_length(root->processed_groupClause);
#else
			numGroupCols = list_length(root->parse->groupClause);
#endif

#if PG_VERSION_NUM >= 140000
			numGroups = estimate_num_groups(root,
#if PG_VERSION_NUM >= 160000
											get_sortgrouplist_exprs(root->processed_groupClause,
#else
											get_sortgrouplist_exprs(root->parse->groupClause,
#endif
																	fpinfo->grouped_tlist),
											input_rows, NULL, NULL);
#else
			numGroups = estimate_num_groups(root,
											get_sortgrouplist_exprs(root->parse->groupClause,
																	fpinfo->grouped_tlist),
											input_rows, NULL);
#endif

			/*
			 * Get the retrieved_rows and rows estimates.  If there are HAVING
			 * quals, account for their selectivity.
			 */
			if (root->hasHavingQual)
			{
				/* Factor in the selectivity of the remotely-checked quals */
				retrieved_rows =
					clamp_row_est(numGroups *
								  clauselist_selectivity(root,
														 fpinfo->remote_conds,
														 0,
														 JOIN_INNER,
														 NULL));
				/* Factor in the selectivity of the locally-checked quals */
				rows = clamp_row_est(retrieved_rows * fpinfo->local_conds_sel);
			}
			else
			{
				rows = retrieved_rows = numGroups;
			}

			/* Use width estimate made by the core code. */
			width = foreignrel->reltarget->width;

			/*-----
			 * Startup cost includes:
			 *	  1. Startup cost for underneath input relation, adjusted for
			 *	     tlist replacement by apply_scanjoin_target_to_paths()
			 *	  2. Cost of performing aggregation, per cost_agg()
			 *-----
			 */
			startup_cost = ofpinfo->rel_startup_cost;
			startup_cost += outerrel->reltarget->cost.startup;
			startup_cost += aggcosts.transCost.startup;
			startup_cost += aggcosts.transCost.per_tuple * input_rows;
			startup_cost += aggcosts.finalCost.startup;
			startup_cost += (cpu_operator_cost * numGroupCols) * input_rows;

			/*-----
			 * Run time cost includes:
			 *	  1. Run time cost of underneath input relation, adjusted for
			 *	     tlist replacement by apply_scanjoin_target_to_paths()
			 *	  2. Run time cost of performing aggregation, per cost_agg()
			 *-----
			 */
			run_cost = ofpinfo->rel_total_cost - ofpinfo->rel_startup_cost;
			run_cost += outerrel->reltarget->cost.per_tuple * input_rows;
			run_cost += aggcosts.finalCost.per_tuple * numGroups;
			run_cost += cpu_tuple_cost * numGroups;

			/* Account for the eval cost of HAVING quals, if any */
			if (root->hasHavingQual)
			{
				QualCost	remote_cost;

				/* Add in the eval cost of the remotely-checked quals */
				cost_qual_eval(&remote_cost, fpinfo->remote_conds, root);
				startup_cost += remote_cost.startup;
				run_cost += remote_cost.per_tuple * numGroups;
				/* Add in the eval cost of the locally-checked quals */
				startup_cost += fpinfo->local_conds_cost.startup;
				run_cost += fpinfo->local_conds_cost.per_tuple * retrieved_rows;
			}

			/* Add in tlist eval cost for each output row */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}
		else
		{
			Cost		cpu_per_tuple;

			/* Use rows/width estimates made by set_baserel_size_estimates. */
			rows = foreignrel->rows;
			width = foreignrel->reltarget->width;

			/*
			 * Back into an estimate of the number of retrieved rows.  Just in
			 * case this is nuts, clamp to at most foreignrel->tuples.
			 */
			retrieved_rows = clamp_row_est(rows / fpinfo->local_conds_sel);
			retrieved_rows = Min(retrieved_rows, foreignrel->tuples);

			/*
			 * Cost as though this were a seqscan, which is pessimistic.  We
			 * effectively imagine the local_conds are being evaluated
			 * remotely, too.
			 */
			startup_cost = 0;
			run_cost = 0;
			run_cost += seq_page_cost * foreignrel->pages;

			startup_cost += foreignrel->baserestrictcost.startup;
			cpu_per_tuple = cpu_tuple_cost + foreignrel->baserestrictcost.per_tuple;
			run_cost += cpu_per_tuple * foreignrel->tuples;

			/* Add in tlist eval cost for each output row */
			startup_cost += foreignrel->reltarget->cost.startup;
			run_cost += foreignrel->reltarget->cost.per_tuple * rows;
		}

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
			if (IS_UPPER_REL(foreignrel))
			{
				Assert(foreignrel->reloptkind == RELOPT_UPPER_REL &&
					   fpinfo->stage == UPPERREL_GROUP_AGG);
				adjust_foreign_grouping_path_cost(root, pathkeys,
												  retrieved_rows, width,
												  fpextra->limit_tuples,
												  &startup_cost, &run_cost);
			}
			else
			{
				startup_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
				run_cost *= DEFAULT_FDW_SORT_MULTIPLIER;
			}
		}

		total_cost = startup_cost + run_cost;

		/* Adjust the cost estimates if we have LIMIT */
		if (fpextra && fpextra->has_limit)
		{
			adjust_limit_rows_costs(&rows, &startup_cost, &total_cost,
									fpextra->offset_est, fpextra->count_est);
			retrieved_rows = rows;
		}
	}

	/*
	 * If this includes the final sort step, the given target, which will be
	 * applied to the resulting path, might have different expressions from
	 * the foreignrel's reltarget (see make_sort_input_target()); adjust tlist
	 * eval costs.
	 */
	if (fpextra && fpextra->has_final_sort &&
		fpextra->target != foreignrel->reltarget)
	{
		QualCost	oldcost = foreignrel->reltarget->cost;
		QualCost	newcost = fpextra->target->cost;

		startup_cost += newcost.startup - oldcost.startup;
		total_cost += newcost.startup - oldcost.startup;
		total_cost += (newcost.per_tuple - oldcost.per_tuple) * rows;
	}

	/*
	 * Cache the retrieved rows and cost estimates for scans, joins, or
	 * groupings without any parameterization, pathkeys, or additional
	 * post-scan/join-processing steps, before adding the costs for
	 * transferring data from the foreign server.  These estimates are useful
	 * for costing remote joins involving this relation or costing other
	 * remote operations on this relation such as remote sorts and remote
	 * LIMIT restrictions, when the costs can not be obtained from the foreign
	 * server.  This function will be called at least once for every foreign
	 * relation without any parameterization, pathkeys, or additional
	 * post-scan/join-processing steps.
	 */
	if (pathkeys == NIL && param_join_conds == NIL && fpextra == NULL)
	{
		fpinfo->retrieved_rows = retrieved_rows;
		fpinfo->rel_startup_cost = startup_cost;
		fpinfo->rel_total_cost = total_cost;
	}

	/*
	 * Add some additional cost factors to account for connection overhead
	 * (fdw_startup_cost), transferring data across the network
	 * (fdw_tuple_cost per retrieved row), and local manipulation of the data
	 * (cpu_tuple_cost per retrieved row).
	 */
	startup_cost += fpinfo->fdw_startup_cost;
	total_cost += fpinfo->fdw_startup_cost;
	total_cost += fpinfo->fdw_tuple_cost * retrieved_rows;
	total_cost += cpu_tuple_cost * retrieved_rows;

	/*
	 * If we have LIMIT, we should prefer performing the restriction remotely
	 * rather than locally, as the former avoids extra row fetches from the
	 * remote that the latter might cause.  But since the core code doesn't
	 * account for such fetches when estimating the costs of the local
	 * restriction (see create_limit_path()), there would be no difference
	 * between the costs of the local restriction and the costs of the remote
	 * restriction estimated above if we don't use remote estimates (except
	 * for the case where the foreignrel is a grouping relation, the given
	 * pathkeys is not NIL, and the effects of a bounded sort for that rel is
	 * accounted for in costing the remote restriction).  Tweak the costs of
	 * the remote restriction to ensure we'll prefer it if LIMIT is a useful
	 * one.
	 */
	if (!fpinfo->use_remote_estimate &&
		fpextra && fpextra->has_limit &&
		fpextra->limit_tuples > 0 &&
		fpextra->limit_tuples < fpinfo->rows)
	{
		Assert(fpinfo->rows > 0);
		total_cost -= (total_cost - startup_cost) * 0.05 *
			(fpinfo->rows - fpextra->limit_tuples) / fpinfo->rows;
	}

	/* Return results. */
	*p_rows = rows;
	*p_width = width;
	*p_startup_cost = startup_cost;
	*p_total_cost = total_cost;
}

/*
 * Adjust the cost estimates of a foreign grouping path to include the cost of
 * generating properly-sorted output.
 */
static void
adjust_foreign_grouping_path_cost(PlannerInfo *root,
								  List *pathkeys,
								  double retrieved_rows,
								  double width,
								  double limit_tuples,
								  Cost *p_startup_cost,
								  Cost *p_run_cost)
{
	/*
	 * If the GROUP BY clause isn't sort-able, the plan chosen by the remote
	 * side is unlikely to generate properly-sorted output, so it would need
	 * an explicit sort; adjust the given costs with cost_sort().  Likewise,
	 * if the GROUP BY clause is sort-able but isn't a superset of the given
	 * pathkeys, adjust the costs with that function.  Otherwise, adjust the
	 * costs by applying the same heuristic as for the scan or join case.
	 */
#if PG_VERSION_NUM < 160000	
	if (!grouping_is_sortable(root->parse->groupClause) ||
#else
	if (!grouping_is_sortable(root->processed_groupClause) ||
#endif
		!pathkeys_contained_in(pathkeys, root->group_pathkeys))
	{
		Path		sort_path;	/* dummy for result of cost_sort */

		cost_sort(&sort_path,
				  root,
				  pathkeys,
				  *p_startup_cost + *p_run_cost,
				  retrieved_rows,
				  width,
				  0.0,
				  work_mem,
				  limit_tuples);

		*p_startup_cost = sort_path.startup_cost;
		*p_run_cost = sort_path.total_cost - sort_path.startup_cost;
	}
	else
	{
		/*
		 * The default extra cost seems too large for foreign-grouping cases;
		 * add 1/4th of that default.
		 */
		double		sort_multiplier = 1.0 + (DEFAULT_FDW_SORT_MULTIPLIER
											 - 1.0) * 0.25;

		*p_startup_cost *= sort_multiplier;
		*p_run_cost *= sort_multiplier;
	}
}

/*
 * Detect whether we want to process an EquivalenceClass member.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel,
						  EquivalenceClass *ec, EquivalenceMember *em,
						  void *arg)
{
	ec_member_foreign_arg *state = (ec_member_foreign_arg *) arg;
	Expr	   *expr = em->em_expr;

	/*
	 * If we've identified what we're processing in the current scan, we only
	 * want to match that expression.
	 */
	if (state->current != NULL)
		return equal(expr, state->current);

	/*
	 * Otherwise, ignore anything we've already processed.
	 */
	if (list_member(state->already_used, expr))
		return false;

	/* This is the new target to process. */
	state->current = expr;
	return true;
}

/*
 * Fetch some more rows from the node's cursor.
 */
static void
fetch_more_data(ForeignScanState *node)
{
	MonetdbFdwScanState *fsstate = (MonetdbFdwScanState *) node->fdw_state;
	MemoryContext oldcontext;

	/*
	 * We'll store the tuples in the batch_cxt.  First, flush the previous
	 * batch.
	 */
	fsstate->tuples = NULL;
	MemoryContextReset(fsstate->batch_cxt);
	oldcontext = MemoryContextSwitchTo(fsstate->batch_cxt);

	/* PGresult must be released before leaving this function. */
	PG_TRY();
	{
		int			numrows;
		int			i;

		/* Convert the data into HeapTuples */
		numrows = mapi_get_row_count(fsstate->hdl);
		fsstate->tuples = (HeapTuple *) palloc0(numrows * sizeof(HeapTuple));
		fsstate->num_tuples = numrows;
		fsstate->next_tuple = 0;

		for (i = 0; i < numrows; i++)
		{
			Assert(IsA(node->ss.ps.plan, ForeignScan));

			fsstate->tuples[i] =
				make_tuple_from_result_row(fsstate->hdl, i,
										   fsstate->rel,
										   fsstate->attinmeta,
										   fsstate->retrieved_attrs,
										   node,
										   fsstate->temp_cxt);
		}

		/* Update fetch_ct_2 */
		if (fsstate->fetch_ct_2 < 2)
			fsstate->fetch_ct_2++;

		/* Must be EOF if we didn't get as many tuples as we asked for. */
		fsstate->eof_reached = (numrows < fsstate->fetch_size);
	}
	PG_FINALLY();
	{

	}
	PG_END_TRY();

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Force assorted GUC parameters to settings that ensure that we'll output
 * data values in a form that is unambiguous to the remote server.
 *
 * This is rather expensive and annoying to do once per row, but there's
 * little choice if we want to be sure values are transmitted accurately;
 * we can't leave the settings in place between rows for fear of affecting
 * user-visible computations.
 *
 * We use the equivalent of a function SET option to allow the settings to
 * persist only until the caller calls reset_transmission_modes().  If an
 * error is thrown in between, guc.c will take care of undoing the settings.
 *
 * The return value is the nestlevel that must be passed to
 * reset_transmission_modes() to undo things.
 */
int
set_transmission_modes(void)
{
	int			nestlevel = NewGUCNestLevel();

	/*
	 * The values set here should match what pg_dump does.  See also
	 * configure_remote_session in connection.c.
	 */
	if (DateStyle != USE_ISO_DATES)
		(void) set_config_option("datestyle", "ISO",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (IntervalStyle != INTSTYLE_POSTGRES)
		(void) set_config_option("intervalstyle", "postgres",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);
	if (extra_float_digits < 3)
		(void) set_config_option("extra_float_digits", "3",
								 PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SAVE, true, 0, false);

	/*
	 * In addition force restrictive search_path, in case there are any
	 * regproc or similar constants to be printed.
	 */
	(void) set_config_option("search_path", "pg_catalog",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	return nestlevel;
}

/*
 * Undo the effects of set_transmission_modes().
 */
void
reset_transmission_modes(int nestlevel)
{
	AtEOXact_GUC(true, nestlevel);
}

/*
 * Prepare for processing of parameters used in remote query.
 */
static void
prepare_query_params(PlanState *node,
					 List *fdw_exprs,
					 int numParams,
					 FmgrInfo **param_flinfo,
					 List **param_exprs,
					 const char ***param_values)
{
	int			i;
	ListCell   *lc;

	Assert(numParams > 0);

	/* Prepare for output conversion of parameters used in remote query. */
	*param_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * numParams);

	i = 0;
	foreach(lc, fdw_exprs)
	{
		Node	   *param_expr = (Node *) lfirst(lc);
		Oid			typefnoid;
		bool		isvarlena;

		getTypeOutputInfo(exprType(param_expr), &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &(*param_flinfo)[i]);
		i++;
	}

	/*
	 * Prepare remote-parameter expressions for evaluation.  (Note: in
	 * practice, we expect that all these expressions will be just Params, so
	 * we could possibly do something more efficient than using the full
	 * expression-eval machinery for this.  But probably there would be little
	 * benefit, and it'd require monetdb_fdw to know more than is desirable
	 * about Param evaluation.)
	 */
	*param_exprs = ExecInitExprList(fdw_exprs, node);

	/* Allocate buffer for text form of query parameters. */
	*param_values = (const char **) palloc0(numParams * sizeof(char *));
}

/*
 * MonetDB_AnalyzeForeignTable
 *		Test whether analyzing this foreign table is supported
 */
static bool
MonetDB_AnalyzeForeignTable(Relation relation,
							AcquireSampleRowsFunc *func,
							BlockNumber *totalpages)
{
	elog(ERROR, "MonetDB_AnalyzeForeignTable not supported yet");
}

/*
 * Import a foreign schema
 */
static List *
MonetDB_ImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	List	   *commands = NIL;
	ForeignServer *server;
	UserMapping *mapping;
	Mapi 	   	conn;
	MapiHdl 	hdl;
	StringInfoData buf;
	int			numrows,
				i;
	ListCell   *lc;

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	server = GetForeignServer(serverOid);
	mapping = GetUserMapping(GetUserId(), server->serverid);
	conn = GetConnection(mapping, server);

	/* Create workspace for strings */
	initStringInfo(&buf);

	/* Check that the schema really exists */
	appendStringInfoString(&buf, "SELECT 1 FROM sys.schemas WHERE name = ");
	deparseStringLiteral(&buf, stmt->remote_schema);
	elog(DEBUG2, "monetdb_fdw remote query is: %s", buf.data);
	if ((hdl = mapi_query(conn, buf.data)) == NULL || mapi_error(conn))
		die(conn, hdl);

	if (mapi_get_row_count(hdl) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_SCHEMA_NOT_FOUND),
					errmsg("schema \"%s\" is not present on foreign server \"%s\"",
						stmt->remote_schema, server->servername)));

	if (mapi_close_handle(hdl) != MOK)
		die(conn, hdl);
	resetStringInfo(&buf);

	/*
	* Fetch all table data from this schema, possibly restricted by
	* EXCEPT or LIMIT TO.  (We don't actually need to pay any attention
	* to EXCEPT/LIMIT TO here, because the core code will filter the
	* statements we return according to those lists anyway.  But it
	* should save a few cycles to not process excluded tables in the
	* first place.)
	*
	* Import table data for partitions only when they are explicitly
	* specified in LIMIT TO clause. Otherwise ignore them and only
	* include the definitions of the root partitioned tables to allow
	* access to the complete remote data set locally in the schema
	* imported.
	*/
	appendStringInfoString(&buf,
							"SELECT t.name as table_name, \n"
							"  c.name as col_name, \n"
							"  sys.sql_datatype(c.type, c.type_digits, c.type_scale, false, false) as type, \n"
							"  CASE WHEN c.\"null\" THEN 'false' ELSE 'true' END as expr, \n"
							"  sys.ifthenelse(c.\"default\" IS NOT NULL, c.\"default\", NULL) as default_expr, \n");

	appendStringInfoString(&buf, " (SELECT true FROM sys.objects kc, sys.keys k where kc.id = k.id and k.table_id = t.id and kc.name = c.name AND k.type = 0) AS pk, \n");

	appendStringInfoString(&buf, " c.number as attnum \n");

	appendStringInfoString(&buf, " FROM sys.tables t, sys.schemas s, sys.columns c  \n");

	appendStringInfoString(&buf,
							" WHERE t.schema_id = s.id \n"
							"  AND t.type = 0 \n"
							"  AND c.table_id = t.id \n"
							"  AND s.name = ");
	deparseStringLiteral(&buf, stmt->remote_schema);

	/* Apply restrictions for LIMIT TO and EXCEPT */
	if (stmt->list_type == FDW_IMPORT_SCHEMA_LIMIT_TO ||
		stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
	{
		bool		first_item = true;

		appendStringInfoString(&buf, " AND t.name ");
		if (stmt->list_type == FDW_IMPORT_SCHEMA_EXCEPT)
			appendStringInfoString(&buf, "NOT ");
		appendStringInfoString(&buf, "IN (");

		/* Append list of table names within IN clause */
		foreach(lc, stmt->table_list)
		{
			RangeVar   *rv = (RangeVar *) lfirst(lc);

			if (first_item)
				first_item = false;
			else
				appendStringInfoString(&buf, ", ");
			deparseStringLiteral(&buf, rv->relname);
		}
		appendStringInfoChar(&buf, ')');
	}

	/* Append ORDER BY at the end of query to ensure output ordering */
	appendStringInfoString(&buf, " ORDER BY table_name, attnum");

	/* Fetch the data */
	elog(DEBUG2, "monetdb_fdw remote query is: \n%s", buf.data);
	if ((hdl = mapi_query(conn, buf.data)) == NULL || mapi_error(conn))
		die(conn, hdl);

	/* Process results */
	numrows = mapi_get_row_count(hdl);

	/* Fetch first row */
	if ((mapi_fetch_row(hdl)) == 0)
		die(conn, hdl);

	/* note: incrementation of i happens in inner loop's while() test */
	for (i = 0; i < numrows;)
	{
		char	   *tablename = NULL;
		bool		first_item = true;

		tablename = mapi_fetch_field(hdl, 0);
		resetStringInfo(&buf);
		appendStringInfo(&buf, "CREATE FOREIGN TABLE %s (\n", quote_identifier(tablename));

		/* Scan all rows for this table */
		do
		{
			char	*attname 	= mapi_fetch_field(hdl, 1);
			char	*typename 	= mapi_fetch_field(hdl, 2);
			char	*attnotnull = mapi_fetch_field(hdl, 3);
			char	*attdefault = mapi_fetch_field(hdl, 4);
			char	*attispk 	= mapi_fetch_field(hdl, 5);

			/* 
			 * At import time, if the MonetDB type may not be supported by PostgreSQL, 
			 * then we should do the type mapping in advance, 
			 * either by ignoring precision or by repointing to the most appropriate data type. 
			 */
			if (!strncmp("JSON", typename, strlen("JSON")))
				typename = pstrdup("JSON");
			else if (!strncmp("URL", typename, strlen("URL")))
				typename = pstrdup("URL");

			if (first_item)
				first_item = false;
			else
				appendStringInfoString(&buf, ",\n");

			/* Print column name and type */
			appendStringInfo(&buf, "  %s %s",
								quote_identifier(attname),
								typename);

			/* Add DEFAULT if needed */
			if (attdefault != NULL)
				appendStringInfo(&buf, " DEFAULT %s", attdefault);

			/* part of the primary key */
			if (attispk && attispk[0] == 't')
				appendStringInfoString(&buf, " OPTIONS (key 'true') ");

			/* Add NOT NULL if needed */
			if (attnotnull[0] == 't')	
				appendStringInfoString(&buf, " NOT NULL");

		}
		while (++i < numrows && (mapi_fetch_row(hdl)) &&
				strcmp(mapi_fetch_field(hdl, 0), tablename) == 0);

		/*
		* Add server name and table-level options.  We specify remote
		* schema and table name as options (the latter to ensure that
		* renaming the foreign table doesn't break the association).
		*/
		appendStringInfo(&buf, "\n) SERVER %s\nOPTIONS (",
							quote_identifier(server->servername));

		appendStringInfoString(&buf, "schema_name ");
		deparseStringLiteral(&buf, stmt->remote_schema);
		appendStringInfoString(&buf, ", table_name ");
		deparseStringLiteral(&buf, tablename);
		appendStringInfoString(&buf, ");");

		commands = lappend(commands, pstrdup(buf.data));
		elog(DEBUG2, "postgres execute query is: \n%s", buf.data);
	}

	return commands;
}

static void
add_paths_with_pathkeys_for_rel(PlannerInfo *root, RelOptInfo *rel,
								Path *epq_path)
{
	List	   *useful_pathkeys_list = NIL; /* List of all pathkeys */
	ListCell   *lc;

	useful_pathkeys_list = get_useful_pathkeys_for_relation(root, rel);

	/*
	 * Before creating sorted paths, arrange for the passed-in EPQ path, if
	 * any, to return columns needed by the parent ForeignScan node so that
	 * they will propagate up through Sort nodes injected below, if necessary.
	 */
	if (epq_path != NULL && useful_pathkeys_list != NIL)
	{
		MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) rel->fdw_private;
		PathTarget *target = copy_pathtarget(epq_path->pathtarget);

		/* Include columns required for evaluating PHVs in the tlist. */
		add_new_columns_to_pathtarget(target,
									  pull_var_clause((Node *) target->exprs,
													  PVC_RECURSE_PLACEHOLDERS));

		/* Include columns required for evaluating the local conditions. */
		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			add_new_columns_to_pathtarget(target,
										  pull_var_clause((Node *) rinfo->clause,
														  PVC_RECURSE_PLACEHOLDERS));
		}

		/*
		 * If we have added any new columns, adjust the tlist of the EPQ path.
		 *
		 * Note: the plan created using this path will only be used to execute
		 * EPQ checks, where accuracy of the plan cost and width estimates
		 * would not be important, so we do not do set_pathtarget_cost_width()
		 * for the new pathtarget here.  See also MonetDB_GetForeignPlan().
		 */
		if (list_length(target->exprs) > list_length(epq_path->pathtarget->exprs))
		{
			/* The EPQ path is a join path, so it is projection-capable. */
			Assert(is_projection_capable_path(epq_path));

			/*
			 * Use create_projection_path() here, so as to avoid modifying it
			 * in place.
			 */
			epq_path = (Path *) create_projection_path(root,
													   rel,
													   epq_path,
													   target);
		}
	}

	/* Create one path for each set of pathkeys we found above. */
	foreach(lc, useful_pathkeys_list)
	{
		double		rows;
		int			width;
		Cost		startup_cost;
		Cost		total_cost;
		List	   *useful_pathkeys = lfirst(lc);
		Path	   *sorted_epq_path;

		estimate_path_cost_size(root, rel, NIL, useful_pathkeys, NULL,
								&rows, &width, &startup_cost, &total_cost);

		/*
		 * The EPQ path must be at least as well sorted as the path itself, in
		 * case it gets used as input to a mergejoin.
		 */
		sorted_epq_path = epq_path;
		if (sorted_epq_path != NULL &&
			!pathkeys_contained_in(useful_pathkeys,
								   sorted_epq_path->pathkeys))
			sorted_epq_path = (Path *)
				create_sort_path(root,
								 rel,
								 sorted_epq_path,
								 useful_pathkeys,
								 -1.0);

		if (IS_SIMPLE_REL(rel))
			add_path(rel, (Path *)
					 create_foreignscan_path(root, rel,
											 NULL,
											 rows,
											 startup_cost,
											 total_cost,
											 useful_pathkeys,
											 rel->lateral_relids,
											 sorted_epq_path,
											 NIL));
		else
			add_path(rel, (Path *)
					 create_foreign_join_path(root, rel,
											  NULL,
											  rows,
											  startup_cost,
											  total_cost,
											  useful_pathkeys,
											  rel->lateral_relids,
											  sorted_epq_path,
											  NIL));
	}
}

/*
 * Parse options from foreign table and apply them to fpinfo.
 *
 * New options might also require tweaking merge_fdw_options().
 */
static void
apply_table_options(MonetdbFdwRelationInfo *fpinfo)
{
	ListCell   *lc;

	foreach(lc, fpinfo->table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "use_remote_estimate") == 0)
			fpinfo->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(def->defname, "fetch_size") == 0)
			(void) parse_int(defGetString(def), &fpinfo->fetch_size, 0, NULL);
		else if (strcmp(def->defname, "async_capable") == 0)
			fpinfo->async_capable = defGetBoolean(def);
	}
}

/*
 * Merge FDW options from input relations into a new set of options for a join
 * or an upper rel.
 *
 * For a join relation, FDW-specific information about the inner and outer
 * relations is provided using fpinfo_i and fpinfo_o.  For an upper relation,
 * fpinfo_o provides the information for the input relation; fpinfo_i is
 * expected to NULL.
 */
static void
merge_fdw_options(MonetdbFdwRelationInfo *fpinfo,
				  const MonetdbFdwRelationInfo *fpinfo_o,
				  const MonetdbFdwRelationInfo *fpinfo_i)
{
	/* We must always have fpinfo_o. */
	Assert(fpinfo_o);

	/* fpinfo_i may be NULL, but if present the servers must both match. */
	Assert(!fpinfo_i ||
		   fpinfo_i->server->serverid == fpinfo_o->server->serverid);

	/*
	 * Copy the server specific FDW options.  (For a join, both relations come
	 * from the same server, so the server options should have the same value
	 * for both relations.)
	 */
	fpinfo->fdw_startup_cost = fpinfo_o->fdw_startup_cost;
	fpinfo->fdw_tuple_cost = fpinfo_o->fdw_tuple_cost;
	fpinfo->shippable_extensions = fpinfo_o->shippable_extensions;
	fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate;
	fpinfo->fetch_size = fpinfo_o->fetch_size;
	fpinfo->async_capable = fpinfo_o->async_capable;

	/* Merge the table level options from either side of the join. */
	if (fpinfo_i)
	{
		/*
		 * We'll prefer to use remote estimates for this join if any table
		 * from either side of the join is using remote estimates.  This is
		 * most likely going to be preferred since they're already willing to
		 * pay the price of a round trip to get the remote EXPLAIN.  In any
		 * case it's not entirely clear how we might otherwise handle this
		 * best.
		 */
		fpinfo->use_remote_estimate = fpinfo_o->use_remote_estimate ||
			fpinfo_i->use_remote_estimate;

		/*
		 * Set fetch size to maximum of the joining sides, since we are
		 * expecting the rows returned by the join to be proportional to the
		 * relation sizes.
		 */
		fpinfo->fetch_size = Max(fpinfo_o->fetch_size, fpinfo_i->fetch_size);

		/*
		 * We'll prefer to consider this join async-capable if any table from
		 * either side of the join is considered async-capable.  This would be
		 * reasonable because in that case the foreign server would have its
		 * own resources to scan that table asynchronously, and the join could
		 * also be computed asynchronously using the resources.
		 */
		fpinfo->async_capable = fpinfo_o->async_capable ||
			fpinfo_i->async_capable;
	}
}

/*
 * MonetDB_GetForeignJoinPaths
 *		Add possible ForeignPath to joinrel, if join is safe to push down.
 */
static void
MonetDB_GetForeignJoinPaths(PlannerInfo *root,
							RelOptInfo *joinrel,
							RelOptInfo *outerrel,
							RelOptInfo *innerrel,
							JoinType jointype,
							JoinPathExtraData *extra)
{
	elog(ERROR, "MonetDB_GetForeignJoinPaths not supported yet");
}

/*
 * Assess whether the aggregation, grouping and having operations can be pushed
 * down to the foreign server.  As a side effect, save information we obtain in
 * this function to MonetdbFdwRelationInfo of the input relation.
 */
static bool
foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
					Node *havingQual)
{
	Query	   *query = root->parse;
	MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) grouped_rel->fdw_private;
	PathTarget *grouping_target = grouped_rel->reltarget;
	MonetdbFdwRelationInfo *ofpinfo;
	ListCell   *lc;
	int			i;
	List	   *tlist = NIL;

	/* We currently don't support pushing Grouping Sets. */
	if (query->groupingSets)
		return false;

	/* Get the fpinfo of the underlying scan relation. */
	ofpinfo = (MonetdbFdwRelationInfo *) fpinfo->outerrel->fdw_private;

	/*
	 * If underlying scan relation has any local conditions, those conditions
	 * are required to be applied before performing aggregation.  Hence the
	 * aggregate cannot be pushed down.
	 */
	if (ofpinfo->local_conds)
		return false;

	/*
	 * Examine grouping expressions, as well as other expressions we'd need to
	 * compute, and check whether they are safe to push down to the foreign
	 * server.  All GROUP BY expressions will be part of the grouping target
	 * and thus there is no need to search for them separately.  Add grouping
	 * expressions into target list which will be passed to foreign server.
	 *
	 * A tricky fine point is that we must not put any expression into the
	 * target list that is just a foreign param (that is, something that
	 * deparse.c would conclude has to be sent to the foreign server).  If we
	 * do, the expression will also appear in the fdw_exprs list of the plan
	 * node, and setrefs.c will get confused and decide that the fdw_exprs
	 * entry is actually a reference to the fdw_scan_tlist entry, resulting in
	 * a broken plan.  Somewhat oddly, it's OK if the expression contains such
	 * a node, as long as it's not at top level; then no match is possible.
	 */
	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);
		ListCell   *l;

		/*
		 * Check whether this expression is part of GROUP BY clause.  Note we
		 * check the whole GROUP BY clause not just processed_groupClause,
		 * because we will ship all of it, cf. appendGroupByClause.
		 */
		if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause))
		{
			TargetEntry *tle;

			/*
			 * If any GROUP BY expression is not shippable, then we cannot
			 * push down aggregation to the foreign server.
			 */
			if (!is_foreign_expr(root, grouped_rel, expr))
				return false;

			/*
			 * If it would be a foreign param, we can't put it into the tlist,
			 * so we have to fail.
			 */
			if (is_foreign_param(root, grouped_rel, expr))
				return false;

			/*
			 * Pushable, so add to tlist.  We need to create a TLE for this
			 * expression and apply the sortgroupref to it.  We cannot use
			 * add_to_flat_tlist() here because that avoids making duplicate
			 * entries in the tlist.  If there are duplicate entries with
			 * distinct sortgrouprefs, we have to duplicate that situation in
			 * the output tlist.
			 */
			tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
			tle->ressortgroupref = sgref;
			tlist = lappend(tlist, tle);
		}
		else
		{
			/*
			 * Non-grouping expression we need to compute.  Can we ship it
			 * as-is to the foreign server?
			 */
			if (is_foreign_expr(root, grouped_rel, expr) &&
				!is_foreign_param(root, grouped_rel, expr))
			{
				/* Yes, so add to tlist as-is; OK to suppress duplicates */
				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
			else
			{
				/* Not pushable as a whole; extract its Vars and aggregates */
				List	   *aggvars;

				aggvars = pull_var_clause((Node *) expr,
										  PVC_INCLUDE_AGGREGATES);

				/*
				 * If any aggregate expression is not shippable, then we
				 * cannot push down aggregation to the foreign server.  (We
				 * don't have to check is_foreign_param, since that certainly
				 * won't return true for any such expression.)
				 */
				if (!is_foreign_expr(root, grouped_rel, (Expr *) aggvars))
					return false;

				/*
				 * Add aggregates, if any, into the targetlist.  Plain Vars
				 * outside an aggregate can be ignored, because they should be
				 * either same as some GROUP BY column or part of some GROUP
				 * BY expression.  In either case, they are already part of
				 * the targetlist and thus no need to add them again.  In fact
				 * including plain Vars in the tlist when they do not match a
				 * GROUP BY column would cause the foreign server to complain
				 * that the shipped query is invalid.
				 */
				foreach(l, aggvars)
				{
					Expr	   *aggref = (Expr *) lfirst(l);

					if (IsA(aggref, Aggref))
						tlist = add_to_flat_tlist(tlist, list_make1(aggref));
				}
			}
		}

		i++;
	}

	/*
	 * Classify the pushable and non-pushable HAVING clauses and save them in
	 * remote_conds and local_conds of the grouped rel's fpinfo.
	 */
	if (havingQual)
	{
		foreach(lc, (List *) havingQual)
		{
			Expr	   *expr = (Expr *) lfirst(lc);
			RestrictInfo *rinfo;

			/*
			 * Currently, the core code doesn't wrap havingQuals in
			 * RestrictInfos, so we must make our own.
			 */
			Assert(!IsA(expr, RestrictInfo));
			rinfo = make_restrictinfo(root,
									  expr,
									  true,
									  false,
									  false,
#if PG_VERSION_NUM >= 160000
									  false,
#endif
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
			if (is_foreign_expr(root, grouped_rel, expr))
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * If there are any local conditions, pull Vars and aggregates from it and
	 * check whether they are safe to pushdown or not.
	 */
	if (fpinfo->local_conds)
	{
		List	   *aggvars = NIL;

		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			aggvars = list_concat(aggvars,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_INCLUDE_AGGREGATES));
		}

		foreach(lc, aggvars)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			/*
			 * If aggregates within local conditions are not safe to push
			 * down, then we cannot push down the query.  Vars are already
			 * part of GROUP BY clause which are checked above, so no need to
			 * access them again here.  Again, we need not check
			 * is_foreign_param for a foreign aggregate.
			 */
			if (IsA(expr, Aggref))
			{
				if (!is_foreign_expr(root, grouped_rel, expr))
					return false;

				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
		}
	}

	/* Store generated targetlist */
	fpinfo->grouped_tlist = tlist;

	/* Safe to pushdown */
	fpinfo->pushdown_safe = true;

	/*
	 * Set # of retrieved rows and cached relation costs to some negative
	 * value, so that we can detect when they are set to some sensible values,
	 * during one (usually the first) of the calls to estimate_path_cost_size.
	 */
	fpinfo->retrieved_rows = -1;
	fpinfo->rel_startup_cost = -1;
	fpinfo->rel_total_cost = -1;

	/*
	 * Set the string describing this grouped relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.  Note that the decoration we add
	 * to the base relation name mustn't include any digits, or it'll confuse
	 * MonetDB_ExplainForeignScan.
	 */
	fpinfo->relation_name = psprintf("Aggregate on (%s)",
									 ofpinfo->relation_name);

	return true;
}

/*
 * MonetDB_GetForeignUpperPaths
 *		Add paths for post-join operations like aggregation, grouping etc. if
 *		corresponding operations are safe to push down.
 */
static void
MonetDB_GetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
							 RelOptInfo *input_rel, RelOptInfo *output_rel,
							 void *extra)
{
	MonetdbFdwRelationInfo *fpinfo;

	/*
	 * If input rel is not safe to pushdown, then simply return as we cannot
	 * perform any post-join operations on the foreign server.
	 */
	if (!input_rel->fdw_private ||
		!((MonetdbFdwRelationInfo *) input_rel->fdw_private)->pushdown_safe)
		return;

	/* Ignore stages we don't support; and skip any duplicate calls. */
	if ((stage != UPPERREL_GROUP_AGG &&
		 stage != UPPERREL_ORDERED &&
		 stage != UPPERREL_FINAL) ||
		output_rel->fdw_private)
		return;

	fpinfo = (MonetdbFdwRelationInfo *) palloc0(sizeof(MonetdbFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	fpinfo->stage = stage;
	output_rel->fdw_private = fpinfo;

	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
			add_foreign_grouping_paths(root, input_rel, output_rel,
									   (GroupPathExtraData *) extra);
			break;
		case UPPERREL_ORDERED:
			add_foreign_ordered_paths(root, input_rel, output_rel);
			break;
		case UPPERREL_FINAL:
			add_foreign_final_paths(root, input_rel, output_rel,
									(FinalPathExtraData *) extra);
			break;
		default:
			elog(ERROR, "unexpected upper relation: %d", (int) stage);
			break;
	}
}

/*
 * add_foreign_grouping_paths
 *		Add foreign path for grouping and/or aggregation.
 *
 * Given input_rel represents the underlying scan.  The paths are added to the
 * given grouped_rel.
 */
static void
add_foreign_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
						   RelOptInfo *grouped_rel,
						   GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	MonetdbFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	MonetdbFdwRelationInfo *fpinfo = grouped_rel->fdw_private;
	ForeignPath *grouppath;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/* Nothing to be done, if there is no grouping or aggregation required. */
	if (!parse->groupClause && !parse->groupingSets && !parse->hasAggs &&
		!root->hasHavingQual)
		return;

	Assert(extra->patype == PARTITIONWISE_AGGREGATE_NONE ||
		   extra->patype == PARTITIONWISE_AGGREGATE_FULL);

	/* save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * Assess if it is safe to push down aggregation and grouping.
	 *
	 * Use HAVING qual from extra. In case of child partition, it will have
	 * translated Vars.
	 */
	if (!foreign_grouping_ok(root, grouped_rel, extra->havingQual))
		return;

	/*
	 * Compute the selectivity and cost of the local_conds, so we don't have
	 * to do it over again for each path.  (Currently we create just a single
	 * path here, but in future it would be possible that we build more paths
	 * such as pre-sorted paths as in MonetDB_GetForeignPaths and
	 * MonetDB_GetForeignJoinPaths.)  The best we can do for these conditions
	 * is to estimate selectivity on the basis of local statistics.
	 */
	fpinfo->local_conds_sel = clauselist_selectivity(root,
													 fpinfo->local_conds,
													 0,
													 JOIN_INNER,
													 NULL);

	cost_qual_eval(&fpinfo->local_conds_cost, fpinfo->local_conds, root);

	/* Estimate the cost of push down */
	estimate_path_cost_size(root, grouped_rel, NIL, NIL, NULL,
							&rows, &width, &startup_cost, &total_cost);

	/* Now update this information in the fpinfo */
	fpinfo->rows = rows;
	fpinfo->width = width;
	fpinfo->startup_cost = startup_cost;
	fpinfo->total_cost = total_cost;

	/* Create and add foreign path to the grouping relation. */
	grouppath = create_foreign_upper_path(root,
										  grouped_rel,
										  grouped_rel->reltarget,
										  rows,
										  startup_cost,
										  total_cost,
										  NIL,	/* no pathkeys */
										  NULL,
										  NIL); /* no fdw_private */

	/* Add generated path into grouped_rel by add_path(). */
	add_path(grouped_rel, (Path *) grouppath);
}

/*
 * add_foreign_ordered_paths
 *		Add foreign paths for performing the final sort remotely.
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given ordered_rel.
 */
static void
add_foreign_ordered_paths(PlannerInfo *root, RelOptInfo *input_rel,
						  RelOptInfo *ordered_rel)
{
	Query	   *parse = root->parse;
	MonetdbFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	MonetdbFdwRelationInfo *fpinfo = ordered_rel->fdw_private;
	MonetdbFdwPathExtraData *fpextra;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *ordered_path;
	ListCell   *lc;

	/* Shouldn't get here unless the query has ORDER BY */
	Assert(parse->sortClause);

	/* We don't support cases where there are any SRFs in the targetlist */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * If the input_rel is a base or join relation, we would already have
	 * considered pushing down the final sort to the remote server when
	 * creating pre-sorted foreign paths for that relation, because the
	 * query_pathkeys is set to the root->sort_pathkeys in that case (see
	 * standard_qp_callback()).
	 */
	if (input_rel->reloptkind == RELOPT_BASEREL ||
		input_rel->reloptkind == RELOPT_JOINREL)
	{
		Assert(root->query_pathkeys == root->sort_pathkeys);

		/* Safe to push down if the query_pathkeys is safe to push down */
		fpinfo->pushdown_safe = ifpinfo->qp_is_pushdown_safe;

		return;
	}

	/* The input_rel should be a grouping relation */
	Assert(input_rel->reloptkind == RELOPT_UPPER_REL &&
		   ifpinfo->stage == UPPERREL_GROUP_AGG);

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying grouping relation to perform the final sort remotely,
	 * which is stored into the fdw_private list of the resulting path.
	 */

	/* Assess if it is safe to push down the final sort */
	foreach(lc, root->sort_pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *pathkey_ec = pathkey->pk_eclass;

		/*
		 * is_foreign_expr would detect volatile expressions as well, but
		 * checking ec_has_volatile here saves some cycles.
		 */
		if (pathkey_ec->ec_has_volatile)
			return;

		/*
		 * Can't push down the sort if pathkey's opfamily is not shippable.
		 */
		if (!is_shippable(pathkey->pk_opfamily, OperatorFamilyRelationId,
						  fpinfo))
			return;

		/*
		 * The EC must contain a shippable EM that is computed in input_rel's
		 * reltarget, else we can't push down the sort.
		 */
		if (find_em_for_rel_target(root,
								   pathkey_ec,
								   input_rel) == NULL)
			return;
	}

	/* Safe to push down */
	fpinfo->pushdown_safe = true;

	/* Construct MonetdbFdwPathExtraData */
	fpextra = (MonetdbFdwPathExtraData *) palloc0(sizeof(MonetdbFdwPathExtraData));
	fpextra->target = root->upper_targets[UPPERREL_ORDERED];
	fpextra->has_final_sort = true;

	/* Estimate the costs of performing the final sort remotely */
	estimate_path_cost_size(root, input_rel, NIL, root->sort_pathkeys, fpextra,
							&rows, &width, &startup_cost, &total_cost);

	/*
	 * Build the fdw_private list that will be used by MonetDB_GetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 */
#if PG_VERSION_NUM >= 150000
	fdw_private = list_make2(makeBoolean(true), makeBoolean(false));
#else
	fdw_private = list_make2(makeInteger(true), makeInteger(false));
#endif

	/* Create foreign ordering path */
	ordered_path = create_foreign_upper_path(root,
											 input_rel,
											 root->upper_targets[UPPERREL_ORDERED],
											 rows,
											 startup_cost,
											 total_cost,
											 root->sort_pathkeys,
											 NULL,	/* no extra plan */
											 fdw_private);

	/* and add it to the ordered_rel */
	add_path(ordered_rel, (Path *) ordered_path);
}

/*
 * add_foreign_final_paths
 *		Add foreign paths for performing the final processing remotely.
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given final_rel.
 */
static void
add_foreign_final_paths(PlannerInfo *root, RelOptInfo *input_rel,
						RelOptInfo *final_rel,
						FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	MonetdbFdwRelationInfo *ifpinfo = (MonetdbFdwRelationInfo *) input_rel->fdw_private;
	MonetdbFdwRelationInfo *fpinfo = (MonetdbFdwRelationInfo *) final_rel->fdw_private;
	bool		has_final_sort = false;
	List	   *pathkeys = NIL;
	MonetdbFdwPathExtraData *fpextra;
	bool		save_use_remote_estimate = false;
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *final_path;

	/*
	 * Currently, we only support this for SELECT commands
	 */
	if (parse->commandType != CMD_SELECT)
		return;

	/*
	 * No work if there is no FOR UPDATE/SHARE clause and if there is no need
	 * to add a LIMIT node
	 */
	if (!parse->rowMarks && !extra->limit_needed)
		return;

	/* We don't support cases where there are any SRFs in the targetlist */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * Copy foreign table, foreign server, user mapping, FDW options etc.
	 * details from the input relation's fpinfo.
	 */
	fpinfo->table = ifpinfo->table;
	fpinfo->server = ifpinfo->server;
	fpinfo->user = ifpinfo->user;
	merge_fdw_options(fpinfo, ifpinfo, NULL);

	/*
	 * If there is no need to add a LIMIT node, there might be a ForeignPath
	 * in the input_rel's pathlist that implements all behavior of the query.
	 * Note: we would already have accounted for the query's FOR UPDATE/SHARE
	 * (if any) before we get here.
	 */
	if (!extra->limit_needed)
	{
		ListCell   *lc;

		Assert(parse->rowMarks);

		/*
		 * Grouping and aggregation are not supported with FOR UPDATE/SHARE,
		 * so the input_rel should be a base, join, or ordered relation; and
		 * if it's an ordered relation, its input relation should be a base or
		 * join relation.
		 */
		Assert(input_rel->reloptkind == RELOPT_BASEREL ||
			   input_rel->reloptkind == RELOPT_JOINREL ||
			   (input_rel->reloptkind == RELOPT_UPPER_REL &&
				ifpinfo->stage == UPPERREL_ORDERED &&
				(ifpinfo->outerrel->reloptkind == RELOPT_BASEREL ||
				 ifpinfo->outerrel->reloptkind == RELOPT_JOINREL)));

		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			/*
			 * apply_scanjoin_target_to_paths() uses create_projection_path()
			 * to adjust each of its input paths if needed, whereas
			 * create_ordered_paths() uses apply_projection_to_path() to do
			 * that.  So the former might have put a ProjectionPath on top of
			 * the ForeignPath; look through ProjectionPath and see if the
			 * path underneath it is ForeignPath.
			 */
			if (IsA(path, ForeignPath) ||
				(IsA(path, ProjectionPath) &&
				 IsA(((ProjectionPath *) path)->subpath, ForeignPath)))
			{
				/*
				 * Create foreign final path; this gets rid of a
				 * no-longer-needed outer plan (if any), which makes the
				 * EXPLAIN output look cleaner
				 */
				final_path = create_foreign_upper_path(root,
													   path->parent,
													   path->pathtarget,
													   path->rows,
													   path->startup_cost,
													   path->total_cost,
													   path->pathkeys,
													   NULL,	/* no extra plan */
													   NULL);	/* no fdw_private */

				/* and add it to the final_rel */
				add_path(final_rel, (Path *) final_path);

				/* Safe to push down */
				fpinfo->pushdown_safe = true;

				return;
			}
		}

		/*
		 * If we get here it means no ForeignPaths; since we would already
		 * have considered pushing down all operations for the query to the
		 * remote server, give up on it.
		 */
		return;
	}

	Assert(extra->limit_needed);

	/*
	 * If the input_rel is an ordered relation, replace the input_rel with its
	 * input relation
	 */
	if (input_rel->reloptkind == RELOPT_UPPER_REL &&
		ifpinfo->stage == UPPERREL_ORDERED)
	{
		input_rel = ifpinfo->outerrel;
		ifpinfo = (MonetdbFdwRelationInfo *) input_rel->fdw_private;
		has_final_sort = true;
		pathkeys = root->sort_pathkeys;
	}

	/* The input_rel should be a base, join, or grouping relation */
	Assert(input_rel->reloptkind == RELOPT_BASEREL ||
		   input_rel->reloptkind == RELOPT_JOINREL ||
		   (input_rel->reloptkind == RELOPT_UPPER_REL &&
			ifpinfo->stage == UPPERREL_GROUP_AGG));

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying base, join, or grouping relation to perform the final
	 * sort (if has_final_sort) and the LIMIT restriction remotely, which is
	 * stored into the fdw_private list of the resulting path.  (We
	 * re-estimate the costs of sorting the underlying relation, if
	 * has_final_sort.)
	 */

	/*
	 * Assess if it is safe to push down the LIMIT and OFFSET to the remote
	 * server
	 */

	/*
	 * If the underlying relation has any local conditions, the LIMIT/OFFSET
	 * cannot be pushed down.
	 */
	if (ifpinfo->local_conds)
		return;

	/*
	 * If the query has FETCH FIRST .. WITH TIES, 1) it must have ORDER BY as
	 * well, which is used to determine which additional rows tie for the last
	 * place in the result set, and 2) ORDER BY must already have been
	 * determined to be safe to push down before we get here.  So in that case
	 * the FETCH clause is safe to push down with ORDER BY if the remote
	 * server is v13 or later, but if not, the remote query will fail entirely
	 * for lack of support for it.  Since we do not currently have a way to do
	 * a remote-version check (without accessing the remote server), disable
	 * pushing the FETCH clause for now.
	 */
	if (parse->limitOption == LIMIT_OPTION_WITH_TIES)
		return;

	/*
	 * Also, the LIMIT/OFFSET cannot be pushed down, if their expressions are
	 * not safe to remote.
	 */
	if (!is_foreign_expr(root, input_rel, (Expr *) parse->limitOffset) ||
		!is_foreign_expr(root, input_rel, (Expr *) parse->limitCount))
		return;

	/* Safe to push down */
	fpinfo->pushdown_safe = true;

	/* Construct MonetdbFdwPathExtraData */
	fpextra = (MonetdbFdwPathExtraData *) palloc0(sizeof(MonetdbFdwPathExtraData));
	fpextra->target = root->upper_targets[UPPERREL_FINAL];
	fpextra->has_final_sort = has_final_sort;
	fpextra->has_limit = extra->limit_needed;
	fpextra->limit_tuples = extra->limit_tuples;
	fpextra->count_est = extra->count_est;
	fpextra->offset_est = extra->offset_est;

	/*
	 * Estimate the costs of performing the final sort and the LIMIT
	 * restriction remotely.  If has_final_sort is false, we wouldn't need to
	 * execute EXPLAIN anymore if use_remote_estimate, since the costs can be
	 * roughly estimated using the costs we already have for the underlying
	 * relation, in the same way as when use_remote_estimate is false.  Since
	 * it's pretty expensive to execute EXPLAIN, force use_remote_estimate to
	 * false in that case.
	 */
	if (!fpextra->has_final_sort)
	{
		save_use_remote_estimate = ifpinfo->use_remote_estimate;
		ifpinfo->use_remote_estimate = false;
	}
	estimate_path_cost_size(root, input_rel, NIL, pathkeys, fpextra,
							&rows, &width, &startup_cost, &total_cost);
	if (!fpextra->has_final_sort)
		ifpinfo->use_remote_estimate = save_use_remote_estimate;

	/*
	 * Build the fdw_private list that will be used by MonetDB_GetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 */
#if PG_VERSION_NUM >= 150000
	fdw_private = list_make2(makeBoolean(has_final_sort),
							 makeBoolean(extra->limit_needed));
#else
	fdw_private = list_make2(makeInteger(has_final_sort),
							 makeInteger(extra->limit_needed));
#endif

	/*
	 * Create foreign final path; this gets rid of a no-longer-needed outer
	 * plan (if any), which makes the EXPLAIN output look cleaner
	 */
	final_path = create_foreign_upper_path(root,
										   input_rel,
										   root->upper_targets[UPPERREL_FINAL],
										   rows,
										   startup_cost,
										   total_cost,
										   pathkeys,
										   NULL,	/* no extra plan */
										   fdw_private);

	/* and add it to the final_rel */
	add_path(final_rel, (Path *) final_path);
}

/*
 * MonetDB_IsForeignPathAsyncCapable
 *		Check whether a given ForeignPath node is async-capable.
 */
static bool
MonetDB_IsForeignPathAsyncCapable(ForeignPath *path)
{
	elog(ERROR, "MonetDB_IsForeignPathAsyncCapable not supported yet");
}

/*
 * MonetDB_ForeignAsyncRequest
 *		Asynchronously request next tuple from a foreign MonetDB_QL table.
 */
static void
MonetDB_ForeignAsyncRequest(AsyncRequest *areq)
{
	elog(ERROR, "MonetDB_ForeignAsyncRequest not supported yet");
}

/*
 * MonetDB_ForeignAsyncConfigureWait
 *		Configure a file descriptor event for which we wish to wait.
 */
static void
MonetDB_ForeignAsyncConfigureWait(AsyncRequest *areq)
{
	elog(ERROR, "MonetDB_ForeignAsyncConfigureWait not supported yet");
}

/*
 * MonetDB_ForeignAsyncNotify
 *		Fetch some more tuples from a file descriptor that becomes ready,
 *		requesting next tuple.
 */
static void
MonetDB_ForeignAsyncNotify(AsyncRequest *areq)
{
	elog(ERROR, "MonetDB_ForeignAsyncNotify not supported yet");
}


/*
 * Create a tuple from the specified row of the PGresult.
 *
 * rel is the local representation of the foreign table, attinmeta is
 * conversion data for the rel's tupdesc, and retrieved_attrs is an
 * integer list of the table column numbers present in the PGresult.
 * fsstate is the ForeignScan plan node's execution state.
 * temp_context is a working context that can be reset after each tuple.
 *
 * Note: either rel or fsstate, but not both, can be NULL.  rel is NULL
 * if we're processing a remote join, while fsstate is NULL in a non-query
 * context such as ANALYZE, or if we're processing a non-scan query node.
 */
static HeapTuple
make_tuple_from_result_row(MapiHdl res,
						   int row,
						   Relation rel,
						   AttInMetadata *attinmeta,
						   List *retrieved_attrs,
						   ForeignScanState *fsstate,
						   MemoryContext temp_context)
{
	HeapTuple	tuple;
	TupleDesc	tupdesc;
	Datum	   *values;
	bool	   *nulls;
	ConversionLocation errpos;
	ErrorContextCallback errcallback;
	MemoryContext oldcontext;
	ListCell   *lc;
	int			j;

	Assert(row < mapi_get_row_count(res));

	/*
	 * Do the following work in a temp context that we reset after each tuple.
	 * This cleans up not only the data we have direct access to, but any
	 * cruft the I/O functions might leak.
	 */
	oldcontext = MemoryContextSwitchTo(temp_context);

	/*
	 * Get the tuple descriptor for the row.  Use the rel's tupdesc if rel is
	 * provided, otherwise look to the scan node's ScanTupleSlot.
	 */
	if (rel)
		tupdesc = RelationGetDescr(rel);
	else
	{
		Assert(fsstate);
		tupdesc = fsstate->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	}

	values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	nulls = (bool *) palloc(tupdesc->natts * sizeof(bool));
	/* Initialize to nulls for any columns not present in result */
	memset(nulls, true, tupdesc->natts * sizeof(bool));

	/*
	 * Set up and install callback to report where conversion error occurs.
	 */
	errpos.cur_attno = 0;
	errpos.rel = rel;
	errpos.fsstate = fsstate;
	errcallback.callback = conversion_error_callback;
	errcallback.arg = (void *) &errpos;
	errcallback.previous = error_context_stack;
	error_context_stack = &errcallback;

	/* fetch next row */
	if (mapi_fetch_row(res))
	{
		/*
		 * i indexes columns in the relation, j indexes columns in the PGresult.
		 */
		j = 0;
		foreach(lc, retrieved_attrs)
		{
			int			i = lfirst_int(lc);
			char	   *valstr;

			/* fetch next column's textual value */
			valstr = mapi_fetch_field(res, j);

			/*
			 * convert value to internal representation
			 */
			errpos.cur_attno = i;
			if (i > 0)
			{
				/* ordinary column */
				Assert(i <= tupdesc->natts);
				nulls[i - 1] = (valstr == NULL);
				/* Apply the input function even to nulls, to support domains */
				values[i - 1] = InputFunctionCall(&attinmeta->attinfuncs[i - 1],
												  valstr,
												  attinmeta->attioparams[i - 1],
												  attinmeta->atttypmods[i - 1]);
			}
			errpos.cur_attno = 0;

			j++;
		}
	}

	/* Uninstall error context callback. */
	error_context_stack = errcallback.previous;

	/*
	 * Check we got the expected number of columns.  Note: j == 0 and
	 * PQnfields == 1 is expected, since deparse emits a NULL if no columns.
	 */
	if (j > 0 && j != mapi_get_field_count(res))
		elog(ERROR, "remote query result does not match the foreign table");

	/*
	 * Build the result tuple in caller's memory context.
	 */
	MemoryContextSwitchTo(oldcontext);

	tuple = heap_form_tuple(tupdesc, values, nulls);

	/*
	 * Stomp on the xmin, xmax, and cmin fields from the tuple created by
	 * heap_form_tuple.  heap_form_tuple actually creates the tuple with
	 * DatumTupleFields, not HeapTupleFields, but the executor expects
	 * HeapTupleFields and will happily extract system columns on that
	 * assumption.  If we don't do this then, for example, the tuple length
	 * ends up in the xmin field, which isn't what we want.
	 */
	HeapTupleHeaderSetXmax(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetXmin(tuple->t_data, InvalidTransactionId);
	HeapTupleHeaderSetCmin(tuple->t_data, InvalidTransactionId);

	/* Clean up */
	MemoryContextReset(temp_context);

	return tuple;
}

/*
 * Callback function which is called when error occurs during column value
 * conversion.  Print names of column and relation.
 *
 * Note that this function mustn't do any catalog lookups, since we are in
 * an already-failed transaction.  Fortunately, we can get the needed info
 * from the relation or the query's rangetable instead.
 */
static void
conversion_error_callback(void *arg)
{
	ConversionLocation *errpos = (ConversionLocation *) arg;
	Relation	rel = errpos->rel;
	ForeignScanState *fsstate = errpos->fsstate;
	const char *attname = NULL;
	const char *relname = NULL;
	bool		is_wholerow = false;

	/*
	 * If we're in a scan node, always use aliases from the rangetable, for
	 * consistency between the simple-relation and remote-join cases.  Look at
	 * the relation's tupdesc only if we're not in a scan node.
	 */
	if (fsstate)
	{
		/* ForeignScan case */
		ForeignScan *fsplan = castNode(ForeignScan, fsstate->ss.ps.plan);
		int			varno = 0;
		AttrNumber	colno = 0;

		if (fsplan->scan.scanrelid > 0)
		{
			/* error occurred in a scan against a foreign table */
			varno = fsplan->scan.scanrelid;
			colno = errpos->cur_attno;
		}
		else
		{
			/* error occurred in a scan against a foreign join */
			TargetEntry *tle;

			tle = list_nth_node(TargetEntry, fsplan->fdw_scan_tlist,
								errpos->cur_attno - 1);

			/*
			 * Target list can have Vars and expressions.  For Vars, we can
			 * get some information, however for expressions we can't.  Thus
			 * for expressions, just show generic context message.
			 */
			if (IsA(tle->expr, Var))
			{
				Var		   *var = (Var *) tle->expr;

				varno = var->varno;
				colno = var->varattno;
			}
		}

		if (varno > 0)
		{
			EState	   *estate = fsstate->ss.ps.state;
			RangeTblEntry *rte = exec_rt_fetch(varno, estate);

			relname = rte->eref->aliasname;

			if (colno == 0)
				is_wholerow = true;
			else if (colno > 0 && colno <= list_length(rte->eref->colnames))
				attname = strVal(list_nth(rte->eref->colnames, colno - 1));
		}
	}
	else if (rel)
	{
		/* Non-ForeignScan case (we should always have a rel here) */
		TupleDesc	tupdesc = RelationGetDescr(rel);

		relname = RelationGetRelationName(rel);
		if (errpos->cur_attno > 0 && errpos->cur_attno <= tupdesc->natts)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc,
												   errpos->cur_attno - 1);

			attname = NameStr(attr->attname);
		}
	}

	if (relname && is_wholerow)
		errcontext("whole-row reference to foreign table \"%s\"", relname);
	else if (relname && attname)
		errcontext("column \"%s\" of foreign table \"%s\"", attname, relname);
	else
		errcontext("processing expression at position %d in select list",
				   errpos->cur_attno);
}

/*
 * Given an EquivalenceClass and a foreign relation, find an EC member
 * that can be used to sort the relation remotely according to a pathkey
 * using this EC.
 *
 * If there is more than one suitable candidate, return an arbitrary
 * one of them.  If there is none, return NULL.
 *
 * This checks that the EC member expression uses only Vars from the given
 * rel and is shippable.  Caller must separately verify that the pathkey's
 * ordering operator is shippable.
 */
EquivalenceMember *
find_em_for_rel(PlannerInfo *root, EquivalenceClass *ec, RelOptInfo *rel)
{
	ListCell   *lc;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);

		/*
		 * Note we require !bms_is_empty, else we'd accept constant
		 * expressions which are not suitable for the purpose.
		 */
		if (bms_is_subset(em->em_relids, rel->relids) &&
			!bms_is_empty(em->em_relids) &&
			is_foreign_expr(root, rel, em->em_expr))
			return em;
	}

	return NULL;
}

/*
 * Find an EquivalenceClass member that is to be computed as a sort column
 * in the given rel's reltarget, and is shippable.
 *
 * If there is more than one suitable candidate, return an arbitrary
 * one of them.  If there is none, return NULL.
 *
 * This checks that the EC member expression uses only Vars from the given
 * rel and is shippable.  Caller must separately verify that the pathkey's
 * ordering operator is shippable.
 */
EquivalenceMember *
find_em_for_rel_target(PlannerInfo *root, EquivalenceClass *ec,
					   RelOptInfo *rel)
{
	PathTarget *target = rel->reltarget;
	ListCell   *lc1;
	int			i;

	i = 0;
	foreach(lc1, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		Index		sgref = get_pathtarget_sortgroupref(target, i);
		ListCell   *lc2;

		/* Ignore non-sort expressions */
		if (sgref == 0 ||
			get_sortgroupref_clause_noerr(sgref,
										  root->parse->sortClause) == NULL)
		{
			i++;
			continue;
		}

		/* We ignore binary-compatible relabeling on both ends */
		while (expr && IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;

		/* Locate an EquivalenceClass member matching this expr, if any */
		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);
			Expr	   *em_expr;

			/* Don't match constants */
			if (em->em_is_const)
				continue;

			/* Ignore child members */
			if (em->em_is_child)
				continue;

			/* Match if same expression (after stripping relabel) */
			em_expr = em->em_expr;
			while (em_expr && IsA(em_expr, RelabelType))
				em_expr = ((RelabelType *) em_expr)->arg;

			if (!equal(em_expr, expr))
				continue;

			/* Check that expression (including relabels!) is shippable */
			if (is_foreign_expr(root, rel, em->em_expr))
				return em;
		}

		i++;
	}

	return NULL;
}

static MonetdbFdwModifyState *create_foreign_modify(EState *estate,
													RangeTblEntry *rte,
													ResultRelInfo *resultRelInfo,
													CmdType operation,
													Plan *subplan,
													char *query,
													List *target_attrs,
													int values_end,
													bool has_returning,
													List *retrieved_attrs)
{
	MonetdbFdwModifyState *fmstate;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	TupleDesc	tupdesc = RelationGetDescr(rel);
	Oid			userid;
	ForeignTable *table;
	UserMapping *user;
	AttrNumber	n_params;
	Oid			typefnoid;
	bool		isvarlena;
	ListCell   *lc;
	ForeignServer *server = NULL;

	/* Begin constructing MonetdbFdwModifyState. */
	fmstate = (MonetdbFdwModifyState *) palloc0(sizeof(MonetdbFdwModifyState));
	fmstate->rel = rel;

	/* Identify which user to do the remote access as. */
#if PG_VERSION_NUM < 160000
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
#else
	userid = ExecGetResultRelCheckAsUser(resultRelInfo, estate);
#endif

	/* Get info about foreign table. */
	table = GetForeignTable(RelationGetRelid(rel));
	user = GetUserMapping(userid, table->serverid);

	/* Open connection; report that we'll create a prepared statement. */
	user = GetUserMapping(userid, table->serverid);
	server = GetForeignServer(table->serverid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	fmstate->conn = GetConnection(user, server);

	fmstate->p_name = NULL;		/* prepared statement not made yet */

	/* Set up remote query information. */
	fmstate->query = query;
	if (operation == CMD_INSERT)
	{
		fmstate->query = pstrdup(fmstate->query);
		fmstate->orig_query = pstrdup(fmstate->query);
	}
	fmstate->target_attrs = target_attrs;
	fmstate->values_end = values_end;
	fmstate->has_returning = has_returning;
	fmstate->retrieved_attrs = retrieved_attrs;

	/* Create context for per-tuple temp workspace. */
	fmstate->temp_cxt = AllocSetContextCreate(estate->es_query_cxt,
											  "monetdb_fdw temporary data",
											  ALLOCSET_SMALL_SIZES);

	/* Prepare for input conversion of RETURNING results. */
	if (fmstate->has_returning)
		fmstate->attinmeta = TupleDescGetAttInMetadata(tupdesc);

	fmstate->p_nums = 0;
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		char		*attrName = NULL;
		Oid 		relid   = RelationGetRelid(rel);
		Oid			attrtype = InvalidOid;
		int			keynum = 0;
		AttrNumber	keyAttno;
		Assert(subplan != NULL);
		
		/* There may be some waste here but that's okay */
		fmstate->key_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * tupdesc->natts);
		/* loop through all columns of the foreign table */
		for (int i = 0; i < tupdesc->natts; ++i)
		{
			Form_pg_attribute att = TupleDescAttr(tupdesc, i);
			ListCell 	*option;

			/* look for the "key" option on this column */
			List 	*option_list = GetForeignColumnOptions(relid, att->attnum);
			foreach(option, option_list)
			{
				DefElem *def = (DefElem *)lfirst(option);

				/* if "key" is set, add a resjunk for this column */
				if (strcmp(def->defname, OPT_KEY) == 0 && getBoolVal(def))
				{
					attrName = pstrdup(NameStr(att->attname));
					attrtype = att->atttypid;

					/* Find the key resjunk column in the subplan's result */
					keyAttno = ExecFindJunkAttributeInTlist(subplan->targetlist,
																	attrName);
					if (!AttributeNumberIsValid(keyAttno))
						elog(ERROR, "could not find junk %s column", attrName);

					fmstate->key_attnums = lappend_int(fmstate->key_attnums, keyAttno);

					/* First transmittable parameter will be key */
					getTypeOutputInfo(attrtype, &typefnoid, &isvarlena);
					fmgr_info(typefnoid, &fmstate->key_flinfo[keynum]);
					++keynum;
				}
			}
		}
		fmstate->p_nums += keynum;
	}

	/* There may be some waste here but that's okay */
	n_params = list_length(fmstate->target_attrs) + list_length(fmstate->key_attnums);
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);

	if (operation == CMD_INSERT || operation == CMD_UPDATE)
	{
		/* Set up for remaining transmittable parameters */
		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			Assert(!attr->attisdropped);

			/* Ignore generated columns; they are set to DEFAULT */
			if (attr->attgenerated)
				continue;
			getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
			fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
			fmstate->p_nums++;
		}
	}

	Assert(fmstate->p_nums <= n_params);

	fmstate->num_slots = 1;

	/* Initialize auxiliary state */
	fmstate->aux_fmstate = NULL;

	return fmstate;
}

static TupleTableSlot **execute_foreign_modify(EState *estate,
												ResultRelInfo *resultRelInfo,
												CmdType operation,
												TupleTableSlot **slots,
												TupleTableSlot **planSlots,
												int *numSlots)
{
	MonetdbFdwModifyState *fmstate = (MonetdbFdwModifyState *) resultRelInfo->ri_FdwState;
	List 			*keylist = NIL;
	const char 		**p_values = NULL;
	int				n_rows = 0;
	StringInfoData 	sql;
	MapiHdl 		result = NULL;

	/* The operation should be INSERT, UPDATE, or DELETE */
	Assert(operation == CMD_INSERT ||
		   operation == CMD_UPDATE ||
		   operation == CMD_DELETE);

	elog(DEBUG2, "monetdb_fdw remote prepare query is: %s", fmstate->query);

	/*
	 * If the existing query was deparsed and prepared for a different number
	 * of rows, rebuild it for the proper number.
	 */
	if (operation == CMD_INSERT && fmstate->num_slots != *numSlots)
	{

		/* Build INSERT string with numSlots records in its VALUES clause. */
		initStringInfo(&sql);
		rebuildInsertSql(&sql, fmstate->rel,
						 fmstate->orig_query, fmstate->target_attrs,
						 fmstate->values_end, fmstate->p_nums,
						 *numSlots - 1);
		pfree(fmstate->query);
		fmstate->query = sql.data;
		fmstate->num_slots = *numSlots;
	}

	/*
	 * For UPDATE/DELETE, get the key that was passed up as a resjunk column
	 */
	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		ListCell	*lc;
		Datum		datum;
		bool		isNull;

		foreach(lc, fmstate->key_attnums)
		{
			AttrNumber		attnum = lfirst_int(lc);
			datum = ExecGetJunkAttribute(planSlots[0],
											attnum,
											&isNull);
			keylist = lappend(keylist, DatumGetPointer(datum));
		}
	}

	/* Convert parameters needed by prepared statement to text form */
	p_values = convert_prep_stmt_params(fmstate, keylist, slots, *numSlots);

	/*
	 * Execute the prepared statement.
	 * Get the result, and check for success.
	 */
	result = mapi_prepare(fmstate->conn, fmstate->query);
	for(int i = 0; i < fmstate->p_nums; i++)
	{
		/* bind value */
		elog(DEBUG2, "monetdb_fdw bind value[%d]: %s", i, (char *) p_values[i]);
		mapi_param_string(result, i, MAPI_VARCHAR, (char *) p_values[i], NULL);
	}

    mapi_execute(result);
	if (result == NULL || mapi_error(fmstate->conn))
		die(fmstate->conn, result);

	n_rows = mapi_get_field_count(result);
	/* Check number of rows affected, and fetch RETURNING tuple if any */
	if (fmstate->has_returning)
	{
		if (n_rows > 0)
		{
			HeapTuple	newtup;
			newtup = make_tuple_from_result_row(result, 0,
												fmstate->rel,
												fmstate->attinmeta,
												fmstate->retrieved_attrs,
												NULL,
												fmstate->temp_cxt);

			/*
			* The returning slot will not necessarily be suitable to store
			* heaptuples directly, so allow for conversion.
			*/
			ExecForceStoreHeapTuple(newtup, slots[0], true);
		}
	}

	MemoryContextReset(fmstate->temp_cxt);

	*numSlots = n_rows;

	/*
	 * Return NULL if nothing was inserted/updated/deleted on the remote end
	 */
	return (n_rows > 0) ? slots : NULL;
}

/*
 * convert_prep_stmt_params
 *		Create array of text strings representing parameter values
 *
 * tupleid is key to send, or NULL if none
 * slot is slot to get remaining parameters from, or NULL if none
 *
 * Data is constructed in temp_cxt; caller should reset that after use.
 */
static const char **
convert_prep_stmt_params(MonetdbFdwModifyState *fmstate,
						 List *tupleid_keys,
						 TupleTableSlot **slots,
						 int numSlots)
{
	const char **p_values;
	int			i;
	int			j;
	int			pindex = 0;
	ListCell   *cell;
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(fmstate->temp_cxt);

	p_values = (const char **) palloc(sizeof(char *) * fmstate->p_nums * numSlots);

	/* key is provided only for UPDATE/DELETE, which don't allow batching */
	Assert(!(list_length(tupleid_keys) != 0 && numSlots > 1));

	/* get parameters from slots */
	if (slots != NULL && fmstate->target_attrs != NIL)
	{
		TupleDesc	tupdesc = RelationGetDescr(fmstate->rel);
		int			nestlevel;
		ListCell   *lc;

		nestlevel = set_transmission_modes();

		for (i = 0; i < numSlots; i++)
		{
			j = list_length(tupleid_keys) ? list_length(tupleid_keys) : 0;
			foreach(lc, fmstate->target_attrs)
			{
				int			attnum = lfirst_int(lc);
				Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
				Datum		value;
				bool		isnull;

				/* Ignore generated columns; they are set to DEFAULT */
				if (attr->attgenerated)
					continue;
				value = slot_getattr(slots[i], attnum, &isnull);
				if (isnull)
					p_values[pindex] = NULL;
				else
				{
					/* 
					 * There are some types of results that are not recognized by MonetDB,
					 * and we need to make simple changes to achieve the purpose
					 */
					char 	*val = OutputFunctionCall(&fmstate->p_flinfo[j], value);
					if (attr->atttypid == BOOLOID)
					{
						/* 
						 * [MonetDB RESULT ERROR] conversion of string 't' to type bit failed. 
						 */
						if (!strcmp(val, "t"))
							val = pstrdup("true");
						else if (!strcmp(val, "f"))
							val = pstrdup("false");
					}
					else if (attr->atttypid == TIMESTAMPTZOID || attr->atttypid == TIMETZOID)
					{
						/* 
						 * [MonetDB RESULT ERROR] Timestamp '2014-04-25 03:12:12.415+08' has incorrect format 
						 * [MonetDB RESULT ERROR] Daytime '17:12:12.415-02' has incorrect format
						 */
						val = psprintf("%s00", val);
					}

					p_values[pindex] = val;
				}
				pindex++;
				j++;
			}
		}

		reset_transmission_modes(nestlevel);
	}

	j = 0;
	foreach(cell, tupleid_keys)
	{
		ItemPointer	attnum = lfirst(cell);
		Assert(numSlots == 1);
		/* Data about the primary key */
		p_values[pindex] = OutputFunctionCall(&fmstate->key_flinfo[j], PointerGetDatum(attnum));
		pindex++;
		j++;
	}

	Assert(pindex == fmstate->p_nums * numSlots);

	MemoryContextSwitchTo(oldcontext);

	return p_values;
}

/*
 * monetdb_execute
 * 		Execute a statement that returns no result values on a foreign server.
 */
PGDLLEXPORT Datum
monetdb_execute(PG_FUNCTION_ARGS)
{
	Name 		srvname = PG_GETARG_NAME(0);
	char 		*stmt   = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Oid 		srvId = InvalidOid;
	int 		fields = 0;
	char 		*line = NULL;
	Mapi 	   	conn;
	MapiHdl 	hdl;
	UserMapping *user = NULL;
	char 		*host = NULL;        
	char 		*port = NULL;         
	char 		*user_str = NULL;          
	char 		*password = NULL;         
	char 		*dbname = NULL;
	List	   	*options = NIL;
	ListCell  	*cell = NULL;
	ForeignServer *server = NULL;
	StringInfoData msg;

	HeapTuple tup = SearchSysCacheCopy1(FOREIGNSERVERNAME, NameGetDatum(srvname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			errmsg("server \"%s\" does not exist", NameStr(*srvname))));

	srvId = ((Form_pg_foreign_server)GETSTRUCT(tup))->oid;
	user = GetUserMapping(GetUserId(), srvId);
	server = GetForeignServer(srvId);
	options = list_concat(options, server->options);
	options = list_concat(options, user->options);

	foreach(cell, options)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (strcmp(def->defname, "host") == 0)
			host = defGetString(def);
		else if (strcmp(def->defname, "port") == 0)
			port = defGetString(def);
		else if (strcmp(def->defname, "user") == 0)
			user_str = defGetString(def);
		else if (strcmp(def->defname, "password") == 0)
			password = defGetString(def);
		else if (strcmp(def->defname, "dbname") == 0)
			dbname = defGetString(def);
	}
	list_free(options);
	
	elog(DEBUG2, "monetdb: host: %s port: %s user: %s password: %s dbname: %s", host, port, user_str, password, dbname);
	elog(DEBUG2, "monetdb_fdw remote query is: %s", stmt);
	conn = mapi_connect(host, atoi(port), user_str, password, "sql", dbname);
	if ((hdl = mapi_query(conn, stmt)) == NULL || mapi_error(conn))
		die(conn, hdl);

	initStringInfo(&msg);
	/* build field_name */
	fields = mapi_get_field_count(hdl);
	for (int i = 0; i < fields; i++)
	{
		char *field_name = mapi_get_name(hdl, i);
		if (field_name)
		{
			if (i == (fields - 1))
				appendStringInfo(&msg, "%s\n", field_name);
			else
				appendStringInfo(&msg, "%s,", field_name);
		}
	}

	/* data */
	while((line = mapi_fetch_line(hdl)) != NULL)
	{
		if (*line != '%')
			appendStringInfo(&msg, "%s\n",  line);
	}

	/* If there are data returned, let's output these data. */
	if (msg.len)
	{
		elog(INFO, "\n%s", msg.data);
		pfree(msg.data);
	}

	/* clear */
	heap_freetuple(tup);
	if (mapi_error(conn))
		error_info(conn, hdl);
	if (mapi_close_handle(hdl) != MOK)
		error_info(conn, hdl);
	mapi_destroy(conn);
	PG_RETURN_VOID();
}

bool
getBoolVal(DefElem *def)
{
	char *s = strVal(def->arg);
	if (pg_strcasecmp(s, "on") == 0
			|| pg_strcasecmp(s, "yes") == 0
			|| pg_strcasecmp(s, "true") == 0)
		return true;
	else if (pg_strcasecmp(s, "off") == 0
			|| pg_strcasecmp(s, "no") == 0
			|| pg_strcasecmp(s, "false") == 0)
		return false;
	else
		ereport(ERROR,
			(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
			 errmsg("invalid value for option \"%s\"", def->defname),
			 errhint("Valid values in this context are: on/yes/true or off/no/false")));
}