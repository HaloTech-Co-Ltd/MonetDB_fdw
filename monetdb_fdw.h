/*-------------------------------------------------------------------------
 *
 * monetdb_fdw.h
 *		  Foreign-data wrapper for remote MonetDB databases
 *
 * Portions Copyright (c) 2025, Halo Tech Co.,Ltd. All rights reserved.
 * Portions Copyright (c) 2012-2023, PostgreSQL Global Development Group
 * 
 * Author: zengman <zengman@halodbtech.com>
 * 
 * IDENTIFICATION
 *		  monetdb_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONETDB_FDW_H
#define MONETDB_FDW_H

#include <mapi.h>

#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "utils/relcache.h"

/* like oracle_fdw */
#define OPT_KEY "key"

#define MAPI_AUTO	0	/* automatic type detection */
#define MAPI_TINY	1
#define MAPI_UTINY	2
#define MAPI_SHORT	3
#define MAPI_USHORT	4
#define MAPI_INT	5
#define MAPI_UINT	6
#define MAPI_LONG	7
#define MAPI_ULONG	8
#define MAPI_LONGLONG	9
#define MAPI_ULONGLONG	10
#define MAPI_CHAR	11
#define MAPI_VARCHAR	12
#define MAPI_FLOAT	13
#define MAPI_DOUBLE	14
#define MAPI_DATE	15
#define MAPI_TIME	16
#define MAPI_DATETIME	17
#define MAPI_NUMERIC	18

#define die(dbh,hdl)																		\
			do {																			\
				if (hdl)																	\
					elog(ERROR, "[MonetDB RESULT ERROR] %s", mapi_result_error(hdl)); 		\
				else if (dbh)																\
					elog(ERROR, "[MonetDB ERROR] %s", mapi_error_str(dbh));					\
				else																		\
					elog(ERROR, "command failed\n"); 										\
			} while (0)

#define error_info(dbh,hdl)																		\
			do {																			\
				if (hdl)																	\
					elog(INFO, "[MonetDB RESULT ERROR] %s", mapi_result_error(hdl)); 		\
				else if (dbh)																\
					elog(INFO, "[MonetDB ERROR] %s", mapi_error_str(dbh));					\
			} while (0)
/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * monetdb_fdw foreign table.  For a baserel, this struct is created by
 * MonetDB_GetForeignRelSize, although some fields are not filled till later.
 * MonetDB_GetForeignJoinPaths creates it for a joinrel, and
 * MonetDB_GetForeignUpperPaths creates it for an upperrel.
 */
typedef struct MonetdbFdwRelationInfo
{
	/*
	 * True means that the relation can be pushed down. Always true for simple
	 * foreign scan.
	 */
	bool		pushdown_safe;

	/*
	 * Restriction clauses, divided into safe and unsafe to pushdown subsets.
	 * All entries in these lists should have RestrictInfo wrappers; that
	 * improves efficiency of selectivity and cost estimation.
	 */
	List	   *remote_conds;
	List	   *local_conds;

	/* Actual remote restriction clauses for scan (sans RestrictInfos) */
	List	   *final_remote_exprs;

	/* Bitmap of attr numbers we need to fetch from the remote server. */
	Bitmapset  *attrs_used;

	/* True means that the query_pathkeys is safe to push down */
	bool		qp_is_pushdown_safe;

	/* Cost and selectivity of local_conds. */
	QualCost	local_conds_cost;
	Selectivity local_conds_sel;

	/* Selectivity of join conditions */
	Selectivity joinclause_sel;

	/* Estimated size and cost for a scan, join, or grouping/aggregation. */
	double		rows;
	int			width;
	Cost		startup_cost;
	Cost		total_cost;

	/*
	 * Estimated number of rows fetched from the foreign server, and costs
	 * excluding costs for transferring those rows from the foreign server.
	 * These are only used by estimate_path_cost_size().
	 */
	double		retrieved_rows;
	Cost		rel_startup_cost;
	Cost		rel_total_cost;

	/* Options extracted from catalogs. */
	bool		use_remote_estimate;
	Cost		fdw_startup_cost;
	Cost		fdw_tuple_cost;
	List	   *shippable_extensions;	/* OIDs of shippable extensions */
	bool		async_capable;

	/* Cached catalog information. */
	ForeignTable *table;
	ForeignServer *server;
	UserMapping *user;			/* only set in use_remote_estimate mode */

	int			fetch_size;		/* fetch size for this remote table */

	/*
	 * Name of the relation, for use while EXPLAINing ForeignScan.  It is used
	 * for join and upper relations but is set for all relations.  For a base
	 * relation, this is really just the RT index as a string; we convert that
	 * while producing EXPLAIN output.  For join and upper relations, the name
	 * indicates which base foreign tables are included and the join type or
	 * aggregation type used.
	 */
	char	   *relation_name;

	/* Join information */
	RelOptInfo *outerrel;
	RelOptInfo *innerrel;
	JoinType	jointype;
	/* joinclauses contains only JOIN/ON conditions for an outer join */
	List	   *joinclauses;	/* List of RestrictInfo */

	/* Upper relation information */
	UpperRelationKind stage;

	/* Grouping information */
	List	   *grouped_tlist;

	/* Subquery information */
	bool		make_outerrel_subquery; /* do we deparse outerrel as a
										 * subquery? */
	bool		make_innerrel_subquery; /* do we deparse innerrel as a
										 * subquery? */
	Relids		lower_subquery_rels;	/* all relids appearing in lower
										 * subqueries */

	/*
	 * Index of the relation.  It is used to create an alias to a subquery
	 * representing the relation.
	 */
	int			relation_index;
} MonetdbFdwRelationInfo;

/* in connection.c */
extern Mapi GetConnection(UserMapping *user, ForeignServer *server);
extern void ReleaseConnection(Mapi conn);
extern void do_sql_command(Mapi conn, const char *sql);

/* in monetdb_fdw.c */
extern int	set_transmission_modes(void);
extern void reset_transmission_modes(int nestlevel);
extern bool getBoolVal(DefElem *def);

/* in deparse.c */
extern void classifyConditions(PlannerInfo *root,
							   RelOptInfo *baserel,
							   List *input_conds,
							   List **remote_conds,
							   List **local_conds);
extern bool is_foreign_expr(PlannerInfo *root,
							RelOptInfo *baserel,
							Expr *expr);
extern bool is_foreign_param(PlannerInfo *root,
							 RelOptInfo *baserel,
							 Expr *expr);
extern bool is_foreign_pathkey(PlannerInfo *root,
							   RelOptInfo *baserel,
							   PathKey *pathkey);
extern void deparseInsertSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *targetAttrs, bool doNothing,
							 List *withCheckOptionList, List *returningList,
							 List **retrieved_attrs, int *values_end_len);
extern void rebuildInsertSql(StringInfo buf, Relation rel,
							 char *orig_query, List *target_attrs,
							 int values_end_len, int num_params,
							 int num_rows);
extern void deparseUpdateSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *targetAttrs,
							 List *withCheckOptionList, List *returningList,
							 List **retrieved_attrs);
extern void deparseDirectUpdateSql(StringInfo buf, PlannerInfo *root,
								   Index rtindex, Relation rel,
								   RelOptInfo *foreignrel,
								   List *targetlist,
								   List *targetAttrs,
								   List *remote_conds,
								   List **params_list,
								   List *returningList,
								   List **retrieved_attrs);
extern void deparseDeleteSql(StringInfo buf, RangeTblEntry *rte,
							 Index rtindex, Relation rel,
							 List *returningList,
							 List **retrieved_attrs);
extern void deparseDirectDeleteSql(StringInfo buf, PlannerInfo *root,
								   Index rtindex, Relation rel,
								   RelOptInfo *foreignrel,
								   List *remote_conds,
								   List **params_list,
								   List *returningList,
								   List **retrieved_attrs);
extern void deparseAnalyzeSizeSql(StringInfo buf, Relation rel);
extern void deparseAnalyzeInfoSql(StringInfo buf, Relation rel);
extern void deparseTruncateSql(StringInfo buf,
							   List *rels,
							   DropBehavior behavior,
							   bool restart_seqs);
extern void deparseStringLiteral(StringInfo buf, const char *val);
extern EquivalenceMember *find_em_for_rel(PlannerInfo *root,
										  EquivalenceClass *ec,
										  RelOptInfo *rel);
extern EquivalenceMember *find_em_for_rel_target(PlannerInfo *root,
												 EquivalenceClass *ec,
												 RelOptInfo *rel);
extern List *build_tlist_to_deparse(RelOptInfo *foreignrel);
extern void deparseSelectStmtForRel(StringInfo buf, PlannerInfo *root,
									RelOptInfo *rel, List *tlist,
									List *remote_conds, List *pathkeys,
									bool has_final_sort, bool has_limit,
									bool is_subquery,
									List **retrieved_attrs, List **params_list);
extern const char *get_jointype_name(JoinType jointype);

/* in shippable.c */
extern bool is_builtin(Oid objectId);
extern bool is_shippable(Oid objectId, Oid classId, MonetdbFdwRelationInfo *fpinfo);

#endif							/* MONETDB_FDW_H */
