/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <access/sysattr.h>
#include <foreign/fdwapi.h>
#include <nodes/extensible.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <nodes/pathnodes.h>
#include <nodes/plannodes.h>
#include <optimizer/appendinfo.h>
#include <optimizer/clauses.h>
#include <optimizer/optimizer.h>
#include <optimizer/pathnode.h>
#include <optimizer/paths.h>
#include <optimizer/prep.h>
#include <optimizer/restrictinfo.h>
#include <optimizer/tlist.h>
#include <parser/parsetree.h>
#include <utils/memutils.h>

#include <math.h>

#include <compat/compat.h>
#include <debug.h>
#include <debug_guc.h>
#include <dimension.h>
#include <export.h>
#include <func_cache.h>
#include <hypertable_cache.h>
#include <import/allpaths.h>
#include <import/planner.h>
#include <planner.h>

#include "data_node_scan_plan.h"

#include "data_node_chunk_assignment.h"
#include "data_node_scan_exec.h"
#include "deparse.h"
#include "fdw_utils.h"
#include "relinfo.h"
#include "scan_plan.h"

/*
 * DataNodeScan is a custom scan implementation for scanning hypertables on
 * remote data nodes instead of scanning individual remote chunks.
 *
 * A DataNodeScan plan is created by taking a regular per-chunk scan plan and
 * then assigning each chunk to a data node, and treating each data node as a
 * "partition" of the distributed hypertable. For each resulting data node, we
 * create a data node rel which is essentially a base rel representing a remote
 * hypertable partition. Since we treat a data node rel as a base rel, although
 * it has no corresponding data node table, we point each data node rel to the root
 * hypertable. This is conceptually the right thing to do, since each data node
 * rel is a partition of the same distributed hypertable.
 *
 * For each data node rel, we plan a DataNodeScan instead of a ForeignScan since a
 * data node rel does not correspond to a real foreign table. A ForeignScan of a
 * data node rel would fail when trying to lookup the ForeignServer via the
 * data node rel's RTE relid. The only other option to get around the
 * ForeignTable lookup is to make a data node rel an upper rel instead of a base
 * rel (see nodeForeignscan.c). However, that leads to other issues in
 * setrefs.c that messes up our target lists for some queries.
 */

static Path *data_node_scan_path_create(PlannerInfo *root, RelOptInfo *rel, PathTarget *target,
										double rows, Cost startup_cost, Cost total_cost,
										List *pathkeys, Relids required_outer, Path *fdw_outerpath,
										List *private);
static Path *data_node_scan_upper_path_create(PlannerInfo *root, RelOptInfo *rel,
											  PathTarget *target, double rows, Cost startup_cost,
											  Cost total_cost, List *pathkeys, Path *fdw_outerpath,
											  List *private);

static AppendRelInfo *
create_append_rel_info(PlannerInfo *root, Index childrelid, Index parentrelid)
{
	RangeTblEntry *parent_rte = planner_rt_fetch(parentrelid, root);
	Relation relation = table_open(parent_rte->relid, NoLock);
	AppendRelInfo *appinfo;

	appinfo = makeNode(AppendRelInfo);
	appinfo->parent_relid = parentrelid;
	appinfo->child_relid = childrelid;
	appinfo->parent_reltype = relation->rd_rel->reltype;
	appinfo->child_reltype = relation->rd_rel->reltype;
	ts_make_inh_translation_list(relation, relation, childrelid, &appinfo->translated_vars);
	appinfo->parent_reloid = parent_rte->relid;
	table_close(relation, NoLock);

	return appinfo;
}

/*
 * Build a new RelOptInfo representing a data node.
 *
 * Note that the relid index should point to the corresponding range table
 * entry (RTE) we created for the data node rel when expanding the
 * hypertable. Each such RTE's relid (OID) refers to the hypertable's root
 * table. This has the upside that the planner can use the hypertable's
 * indexes to plan remote queries more efficiently. In contrast, chunks are
 * foreign tables and they cannot have indexes.
 */
static RelOptInfo *
build_data_node_rel(PlannerInfo *root, Index relid, Oid serverid, RelOptInfo *parent)
{
	RelOptInfo *rel = build_simple_rel(root, relid, parent);

	/*
	 * Use relevant exprs and restrictinfos from the parent rel. These will be
	 * adjusted to match the data node rel's relid later.
	 */
	rel->reltarget->exprs = copyObject(parent->reltarget->exprs);
	rel->baserestrictinfo = parent->baserestrictinfo;
	rel->baserestrictcost = parent->baserestrictcost;
	rel->baserestrict_min_security = parent->baserestrict_min_security;
	rel->lateral_vars = parent->lateral_vars;
	rel->lateral_referencers = parent->lateral_referencers;
	rel->lateral_relids = parent->lateral_relids;
	rel->serverid = serverid;

	/*
	 * We need to use the FDW interface to get called by the planner for
	 * partial aggs. For some reason, the standard upper_paths_hook is never
	 * called for upper rels of type UPPERREL_PARTIAL_GROUP_AGG, which is odd
	 * (see end of PostgreSQL planner.c:create_partial_grouping_paths). Until
	 * this gets fixed in the PostgreSQL planner, we're forced to set
	 * fdwroutine here although we will scan this rel with a DataNodeScan and
	 * not a ForeignScan.
	 */
	rel->fdwroutine = GetFdwRoutineByServerId(serverid);

	return rel;
}

/*
 * Adjust the attributes of data node rel quals.
 *
 * Code adapted from allpaths.c: set_append_rel_size.
 *
 * For each data node child rel, copy the quals/restrictions from the parent
 * (hypertable) rel and adjust the attributes (e.g., Vars) to point to the
 * child rel instead of the parent.
 *
 * Normally, this happens as part of estimating the rel size of an append
 * relation in standard planning, where constraint exclusion and partition
 * pruning also happens for each child. Here, however, we don't prune any
 * data node rels since they are created based on assignment of already pruned
 * chunk child rels at an earlier stage. Data node rels that aren't assigned any
 * chunks will never be created in the first place.
 */
static void
adjust_data_node_rel_attrs(PlannerInfo *root, RelOptInfo *data_node_rel, RelOptInfo *hyper_rel,
						   AppendRelInfo *appinfo)
{
	List *nodequals = NIL;
	ListCell *lc;

	foreach (lc, hyper_rel->baserestrictinfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Node *nodequal;
		ListCell *lc2;

		nodequal = adjust_appendrel_attrs(root, (Node *) rinfo->clause, 1, &appinfo);

		nodequal = eval_const_expressions(root, nodequal);

		/* might have gotten an AND clause, if so flatten it */
		foreach (lc2, make_ands_implicit((Expr *) nodequal))
		{
			Node *onecq = (Node *) lfirst(lc2);
			bool pseudoconstant;

			/* check for pseudoconstant (no Vars or volatile functions) */
			pseudoconstant = !contain_vars_of_level(onecq, 0) && !contain_volatile_functions(onecq);
			if (pseudoconstant)
			{
				/* tell createplan.c to check for gating quals */
				root->hasPseudoConstantQuals = true;
			}
			/* reconstitute RestrictInfo with appropriate properties */
			nodequals = lappend(nodequals,
								make_restrictinfo_compat(root,
														 (Expr *) onecq,
														 rinfo->is_pushed_down,
														 rinfo->outerjoin_delayed,
														 pseudoconstant,
														 rinfo->security_level,
														 NULL,
														 NULL,
														 NULL));
		}
	}

	data_node_rel->baserestrictinfo = nodequals;
	data_node_rel->joininfo =
		castNode(List, adjust_appendrel_attrs(root, (Node *) hyper_rel->joininfo, 1, &appinfo));

	data_node_rel->reltarget->exprs =
		castNode(List,
				 adjust_appendrel_attrs(root, (Node *) hyper_rel->reltarget->exprs, 1, &appinfo));

	/* Add equivalence class for rel to push down joins and sorts */
	if (hyper_rel->has_eclass_joins || has_useful_pathkeys(root, hyper_rel))
		add_child_rel_equivalences(root, appinfo, hyper_rel, data_node_rel);

	data_node_rel->has_eclass_joins = hyper_rel->has_eclass_joins;
}

/*
 * Build RelOptInfos for each data node.
 *
 * Each data node rel will point to the root hypertable table, which is
 * conceptually correct since we query the identical (partial) hypertables on
 * the data nodes.
 */
static RelOptInfo **
build_data_node_part_rels(PlannerInfo *root, RelOptInfo *hyper_rel, int *nparts)
{
	TimescaleDBPrivate *priv = hyper_rel->fdw_private;
	/* Update the partitioning to reflect the new per-data node plan */
	RelOptInfo **part_rels = palloc(sizeof(RelOptInfo *) * list_length(priv->serverids));
	ListCell *lc;
	int n = 0;
	int i;

	Assert(list_length(priv->serverids) == bms_num_members(priv->server_relids));
	i = -1;

	foreach (lc, priv->serverids)
	{
		Oid data_node_id = lfirst_oid(lc);
		RelOptInfo *data_node_rel;
		AppendRelInfo *appinfo;

		i = bms_next_member(priv->server_relids, i);

		Assert(i > 0);

		/*
		 * The planner expects an AppendRelInfo for any part_rels. Needs to be
		 * added prior to creating the rel because build_simple_rel will
		 * invoke our planner hooks that classify relations using this
		 * information.
		 */
		appinfo = create_append_rel_info(root, i, hyper_rel->relid);
		root->append_rel_array[i] = appinfo;
		data_node_rel = build_data_node_rel(root, i, data_node_id, hyper_rel);
		part_rels[n++] = data_node_rel;
		adjust_data_node_rel_attrs(root, data_node_rel, hyper_rel, appinfo);
	}

	if (nparts != NULL)
		*nparts = n;

	return part_rels;
}

/* Callback argument for ts_ec_member_matches_foreign */
typedef struct
{
	Expr *current;		/* current expr, or NULL if not yet found */
	List *already_used; /* expressions already dealt with */
} ts_ec_member_foreign_arg;

/*
 * Detect whether we want to process an EquivalenceClass member.
 *
 * This is a callback for use by generate_implied_equalities_for_column.
 */
static bool
ts_ec_member_matches_foreign(PlannerInfo *root, RelOptInfo *rel, EquivalenceClass *ec,
							 EquivalenceMember *em, void *arg)
{
	ts_ec_member_foreign_arg *state = (ts_ec_member_foreign_arg *) arg;
	Expr *expr = em->em_expr;

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
 * Build parameterizations that are useful for performing joins with the given
 * hypertable relation. We will use them to generate the parameterized data node
 * scan paths. The code is mostly copied from postgres_fdw,
 * postgresGetForeignPaths().
 */
static List *
build_parameterizations(PlannerInfo *root, RelOptInfo *hyper_rel)
{
	/*
	 * Thumb through all join clauses for the rel to identify which outer
	 * relations could supply one or more safe-to-send-to-remote join clauses.
	 * We'll build a parameterized path for each such outer relation.
	 *
	 * Note that in case we have multiple local tables, this outer relation
	 * here may be the result of joining the local tables together. For an
	 * example, see the multiple join in the dist_param test.
	 *
	 * It's convenient to represent each candidate outer relation by the
	 * ParamPathInfo node for it.  We can then use the ppi_clauses list in the
	 * ParamPathInfo node directly as a list of the interesting join clauses for
	 * that rel.  This takes care of the possibility that there are multiple
	 * safe join clauses for such a rel, and also ensures that we account for
	 * unsafe join clauses that we'll still have to enforce locally (since the
	 * parameterized-path machinery insists that we handle all movable clauses).
	 */
	List *ppi_list = NIL;
	ListCell *lc;
	foreach (lc, hyper_rel->joininfo)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Relids required_outer;
		ParamPathInfo *param_info;

		/* Check if clause can be moved to this rel */
		if (!join_clause_is_movable_to(rinfo, hyper_rel))
		{
			continue;
		}

		/* See if it is safe to send to remote */
		if (!ts_is_foreign_expr(root, hyper_rel, rinfo->clause))
		{
			continue;
		}

		/* Calculate required outer rels for the resulting path */
		required_outer = bms_union(rinfo->clause_relids, hyper_rel->lateral_relids);
		/* We do not want the data node rel itself listed in required_outer */
		required_outer = bms_del_member(required_outer, hyper_rel->relid);

		/*
		 * required_outer probably can't be empty here, but if it were, we
		 * couldn't make a parameterized path.
		 */
		if (bms_is_empty(required_outer))
		{
			continue;
		}

		/* Get the ParamPathInfo */
		param_info = get_baserel_parampathinfo(root, hyper_rel, required_outer);
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
	if (hyper_rel->has_eclass_joins)
	{
		/*
		 * We repeatedly scan the eclass list looking for column references
		 * (or expressions) belonging to the data node rel.  Each time we find
		 * one, we generate a list of equivalence joinclauses for it, and then
		 * see if any are safe to send to the remote.  Repeat till there are
		 * no more candidate EC members.
		 */
		ts_ec_member_foreign_arg arg;

		arg.already_used = NIL;
		for (;;)
		{
			List *clauses;

			/* Make clauses, skipping any that join to lateral_referencers */
			arg.current = NULL;
			clauses = generate_implied_equalities_for_column(root,
															 hyper_rel,
															 ts_ec_member_matches_foreign,
															 (void *) &arg,
															 hyper_rel->lateral_referencers);

			/* Done if there are no more expressions in the data node rel */
			if (arg.current == NULL)
			{
				Assert(clauses == NIL);
				break;
			}

			/* Scan the extracted join clauses */
			foreach (lc, clauses)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
				Relids required_outer;
				ParamPathInfo *param_info;

				/* Check if clause can be moved to this rel */
				if (!join_clause_is_movable_to(rinfo, hyper_rel))
				{
					continue;
				}

				/* See if it is safe to send to remote */
				if (!ts_is_foreign_expr(root, hyper_rel, rinfo->clause))
				{
					continue;
				}

				/* Calculate required outer rels for the resulting path */
				required_outer = bms_union(rinfo->clause_relids, hyper_rel->lateral_relids);
				required_outer = bms_del_member(required_outer, hyper_rel->relid);
				if (bms_is_empty(required_outer))
				{
					continue;
				}

				/* Get the ParamPathInfo */
				param_info = get_baserel_parampathinfo(root, hyper_rel, required_outer);
				Assert(param_info != NULL);

				/* Add it to list unless we already have it */
				ppi_list = list_append_unique_ptr(ppi_list, param_info);
			}

			/* Try again, now ignoring the expression we found this time */
			arg.already_used = lappend(arg.already_used, arg.current);
		}
	}

	return ppi_list;
}

static void
add_data_node_scan_paths(PlannerInfo *root, RelOptInfo *data_node_rel, RelOptInfo *hyper_rel,
						 List *ppi_list)
{
	TsFdwRelInfo *fpinfo = fdw_relinfo_get(data_node_rel);
	Path *path;

	if (data_node_rel->reloptkind == RELOPT_JOINREL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign joins are not supported")));

	path = data_node_scan_path_create(root,
									  data_node_rel,
									  NULL, /* default pathtarget */
									  fpinfo->rows,
									  fpinfo->startup_cost,
									  fpinfo->total_cost,
									  NIL, /* no pathkeys */
									  NULL,
									  NULL /* no extra plan */,
									  NIL);

	fdw_utils_add_path(data_node_rel, path);

	/* Add paths with pathkeys */
	fdw_add_paths_with_pathkeys_for_rel(root, data_node_rel, NULL, data_node_scan_path_create);

	/*
	 * Now build a path for each useful outer relation, if the parameterized
	 * data node scans are not disabled.
	 */
	if (!ts_guc_enable_parameterized_data_node_scan)
	{
		return;
	}

	ListCell *ppi_cell;
	foreach (ppi_cell, ppi_list)
	{
		ParamPathInfo *param_info = (ParamPathInfo *) lfirst(ppi_cell);

		/*
		 * Guess if this data node scan is likely to be an index scan, and
		 * factor that into the cost.
		 */
		double scan_cost_discount_multiplier = 1.;
		ListCell *path_cell;
		foreach (path_cell, hyper_rel->pathlist)
		{
			Path *path = (Path *) lfirst(path_cell);
			if (path->param_info == param_info)
			{
				/*
				 * We shouldn't have parameterized seq scans. Can be an
				 * IndexPath (includes index-only scans) or a BitmapHeapPath.
				 */
				Assert(path->type == T_BitmapHeapPath || path->type == T_IndexPath);

				/*
				 * We have an index path with a matching parameterization for
				 * the hypertable rel. This means that we also have a matching
				 * index on the data nodes. As a hack, try to cost this path not
				 * as a seq scan, but as an imaginary index scan that's cheaper
				 * by a constant factor.
				 */
				scan_cost_discount_multiplier = 0.1;
				break;
			}
		}

		/*
		 * As a baseline, cost the data node scan as a seq scan.
		 */
		Cost startup_cost = 0;
		Cost run_cost = 0;
		double rows = data_node_rel->tuples > 1 ? data_node_rel->tuples : 123456;

		/* Run remote non-join clauses. */
		const double remote_sel_sane =
			(fpinfo->remote_conds_sel > 0 && fpinfo->remote_conds_sel <= 1) ?
				fpinfo->remote_conds_sel :
				0.1;

		startup_cost += data_node_rel->reltarget->cost.startup;
		startup_cost += fpinfo->remote_conds_cost.startup;
		run_cost += fpinfo->remote_conds_cost.per_tuple * rows;
		run_cost += cpu_tuple_cost * rows;
		run_cost += seq_page_cost * data_node_rel->pages;
		rows *= remote_sel_sane;

		/* Discount it if it's likely to be an index scan on the remote. */
		run_cost *= scan_cost_discount_multiplier;

		/* Run remote join clauses. */
		QualCost remote_join_cost;
		cost_qual_eval(&remote_join_cost, param_info->ppi_clauses, root);

		/*
		 * We don't have up to date per-column statistics for distributed
		 * hypertables currently, so the join estimates are going to be way off.
		 * The worst is when they are too low and we end up transferring much
		 * more rows from the data node that we expected. Just hardcode it at
		 * 0.1 per clause for now.
		 */
		const double remote_join_sel = pow(0.1, list_length(param_info->ppi_clauses));

		startup_cost += remote_join_cost.startup;
		run_cost += remote_join_cost.per_tuple * rows;
		rows *= remote_join_sel;

		/* Transfer the resulting tuples over the network. */
		startup_cost += fpinfo->fdw_startup_cost;
		run_cost += fpinfo->fdw_tuple_cost * rows;

		/* Run local filters. */
		const double local_sel_sane =
			(fpinfo->local_conds_sel > 0 && fpinfo->local_conds_sel <= 1) ?
				fpinfo->local_conds_sel :
				0.5;

		startup_cost += fpinfo->local_conds_cost.startup;
		run_cost += fpinfo->local_conds_cost.per_tuple * rows;
		run_cost += cpu_tuple_cost * rows;
		rows *= local_sel_sane;

		/* Compute the output targetlist. */
		run_cost += data_node_rel->reltarget->cost.per_tuple * rows;

		rows = clamp_row_est(rows);

		/*
		 * ppi_rows currently won't get looked at by anything, but still we
		 * may as well ensure that it matches our idea of the rowcount.
		 */
		param_info->ppi_rows = rows;

		/* Make the path */
		path = data_node_scan_path_create(root,
										  data_node_rel,
										  NULL, /* default pathtarget */
										  rows,
										  startup_cost,
										  startup_cost + run_cost,
										  NIL, /* no pathkeys */
										  param_info->ppi_req_outer,
										  NULL,
										  NIL); /* no fdw_private list */

		add_path(data_node_rel, (Path *) path);
	}
}

/*
 * Force GROUP BY aggregates to be pushed down.
 *
 * Push downs are forced by making the GROUP BY expression in the query become
 * the partitioning keys, even if this is not compatible with
 * partitioning. This makes the planner believe partitioning and GROUP BYs
 * line up perfectly. Forcing a push down is useful because the PostgreSQL
 * planner is not smart enough to realize it can always push things down if
 * there's, e.g., only one partition (or data node) involved in the query.
 */
static void
force_group_by_push_down(PlannerInfo *root, RelOptInfo *hyper_rel)
{
	PartitionScheme partscheme = hyper_rel->part_scheme;
	List *groupexprs;
	List **nullable_partexprs;
	int16 new_partnatts;
	Oid *partopfamily;
	Oid *partopcintype;
	Oid *partcollation;
	ListCell *lc;
	int i = 0;

	Assert(partscheme != NULL);

	groupexprs = get_sortgrouplist_exprs(root->parse->groupClause, root->parse->targetList);
	new_partnatts = list_length(groupexprs);

	/*
	 * Only reallocate the partitioning attributes arrays if it is smaller than
	 * the new size. palloc0 is needed to zero out the extra space.
	 */
	if (partscheme->partnatts < new_partnatts)
	{
		partopfamily = palloc0(new_partnatts * sizeof(Oid));
		partopcintype = palloc0(new_partnatts * sizeof(Oid));
		partcollation = palloc0(new_partnatts * sizeof(Oid));
		nullable_partexprs = palloc0(new_partnatts * sizeof(List *));

		memcpy(partopfamily, partscheme->partopfamily, partscheme->partnatts * sizeof(Oid));
		memcpy(partopcintype, partscheme->partopcintype, partscheme->partnatts * sizeof(Oid));
		memcpy(partcollation, partscheme->partcollation, partscheme->partnatts * sizeof(Oid));
		memcpy(nullable_partexprs,
			   hyper_rel->nullable_partexprs,
			   partscheme->partnatts * sizeof(List *));

		partscheme->partopfamily = partopfamily;
		partscheme->partopcintype = partopcintype;
		partscheme->partcollation = partcollation;
		hyper_rel->nullable_partexprs = nullable_partexprs;

		hyper_rel->partexprs = (List **) palloc0(sizeof(List *) * new_partnatts);
	}

	partscheme->partnatts = new_partnatts;

	foreach (lc, groupexprs)
	{
		List *expr = lfirst(lc);

		hyper_rel->partexprs[i++] = list_make1(expr);
	}

	Assert(i == partscheme->partnatts);
}

/*
 * Check if it is safe to push down GROUP BYs to remote nodes. A push down is
 * safe if the chunks that are part of the query are disjointedly partitioned
 * on data nodes along the first closed "space" dimension, or all dimensions are
 * covered in the GROUP BY expresssion.
 *
 * If we knew that the GROUP BY covers all partitioning keys, we would not
 * need to check overlaps. Such a check is done in
 * planner.c:group_by_has_partkey(), but this function is not public. We
 * could copy it here to avoid some unnecessary work.
 *
 * There are other "base" cases when we can always safely push down--even if
 * the GROUP BY does NOT cover the partitioning keys--for instance, when only
 * one data node is involved in the query. We try to account for such cases too
 * and "trick" the PG planner to do the "right" thing.
 *
 * We also want to add any bucketing expression (on, e.g., time) as a "meta"
 * partitioning key (in rel->partexprs). This will make the partitionwise
 * planner accept the GROUP BY clause for push down even though the expression
 * on time is a "derived" partitioning key.
 */
static void
push_down_group_bys(PlannerInfo *root, RelOptInfo *hyper_rel, Hyperspace *hs,
					DataNodeChunkAssignments *scas)
{
	const Dimension *dim;
	bool overlaps;

	Assert(hs->num_dimensions >= 1);
	Assert(hyper_rel->part_scheme->partnatts == hs->num_dimensions);

	/*
	 * Check for special case when there is only one data node with chunks. This
	 * can always be safely pushed down irrespective of partitioning
	 */
	if (scas->num_nodes_with_chunks == 1)
	{
		force_group_by_push_down(root, hyper_rel);
		return;
	}

	/*
	 * Get first closed dimension that we use for assigning chunks to
	 * data nodes. If there is no closed dimension, we are done.
	 */
	dim = hyperspace_get_closed_dimension(hs, 0);

	if (NULL == dim)
		return;

	overlaps = data_node_chunk_assignments_are_overlapping(scas, dim->fd.id);

	if (!overlaps)
	{
		/*
		 * If data node chunk assignments are non-overlapping along the
		 * "space" dimension, we can treat this as a one-dimensional
		 * partitioned table since any aggregate GROUP BY that includes the
		 * data node assignment dimension is safe to execute independently on
		 * each data node.
		 */
		Assert(NULL != dim);
		hyper_rel->partexprs[0] = ts_dimension_get_partexprs(dim, hyper_rel->relid);
		hyper_rel->part_scheme->partnatts = 1;
	}
}

/*
 * Turn chunk append paths into data node append paths.
 *
 * By default, a hypertable produces append plans where each child is a chunk
 * to be scanned. This function computes alternative append plans where each
 * child corresponds to a data node.
 *
 * In the future, additional assignment algorithms can create their own
 * append paths and have the cost optimizer pick the best one.
 */
void
data_node_scan_add_node_paths(PlannerInfo *root, RelOptInfo *hyper_rel)
{
	RelOptInfo **chunk_rels = hyper_rel->part_rels;
	int nchunk_rels = hyper_rel->nparts;
	RangeTblEntry *hyper_rte = planner_rt_fetch(hyper_rel->relid, root);
	Cache *hcache = ts_hypertable_cache_pin();
	Hypertable *ht = ts_hypertable_cache_get_entry(hcache, hyper_rte->relid, CACHE_FLAG_NONE);
	List *data_node_rels_list = NIL;
	RelOptInfo **data_node_rels;
#if PG15_GE
	Bitmapset *data_node_live_rels = NULL;
#endif
	int ndata_node_rels;
	DataNodeChunkAssignments scas;
	int i;

	Assert(NULL != ht);

	if (nchunk_rels <= 0)
	{
		ts_cache_release(hcache);
		return;
	}

	/* Create the RelOptInfo for each data node */
	data_node_rels = build_data_node_part_rels(root, hyper_rel, &ndata_node_rels);

	Assert(ndata_node_rels > 0);

	data_node_chunk_assignments_init(&scas, SCA_STRATEGY_ATTACHED_DATA_NODE, root, ndata_node_rels);

	/* Assign chunks to data nodes */
	data_node_chunk_assignment_assign_chunks(&scas, chunk_rels, nchunk_rels);

	/* Try to push down GROUP BY expressions and bucketing, if possible */
	push_down_group_bys(root, hyper_rel, ht->space, &scas);

	/*
	 * Index path for this relation are not useful by themselves, but we are
	 * going to use them to guess whether the remote scan can use an index for a
	 * given parameterization. This is needed to estimate the cost for
	 * parameterized data node scans. We will reset the pathlist below so these
	 * path are not going to be used.
	 */
	create_index_paths(root, hyper_rel);

	/*
	 * Not sure what parameterizations there could be except the ones used for
	 * join. Still, it's hard to verify from the code because
	 * get_baserel_parampathinfo() is called all over the place w/o checking if
	 * a join would be valid for the given required_outer. So for generating
	 * the parameterized data node scan paths we'll use the explicit list of
	 * ppis valid for joins that we just built, and not the entire
	 * hyper_rel->ppilist.
	 */
	List *ppi_list = build_parameterizations(root, hyper_rel);

	/*
	 * Create estimates and paths for each data node rel based on data node chunk
	 * assignments.
	 */
	for (i = 0; i < ndata_node_rels; i++)
	{
		RelOptInfo *data_node_rel = data_node_rels[i];
		DataNodeChunkAssignment *sca =
			data_node_chunk_assignment_get_or_create(&scas, data_node_rel);
		TsFdwRelInfo *fpinfo;

		/*
		 * Basic stats for data node rels come from the assigned chunks since
		 * data node rels don't correspond to real tables in the system.
		 */
		data_node_rel->pages = sca->pages;
		data_node_rel->tuples = sca->tuples;
		data_node_rel->rows = sca->rows;
		/* The width should be the same as any chunk */
		data_node_rel->reltarget->width = hyper_rel->part_rels[0]->reltarget->width;

		fpinfo = fdw_relinfo_create(root,
									data_node_rel,
									data_node_rel->serverid,
									hyper_rte->relid,
									TS_FDW_RELINFO_HYPERTABLE_DATA_NODE);

		fpinfo->sca = sca;

		if (!bms_is_empty(sca->chunk_relids))
		{
			add_data_node_scan_paths(root, data_node_rel, hyper_rel, ppi_list);
			data_node_rels_list = lappend(data_node_rels_list, data_node_rel);
#if PG15_GE
			data_node_live_rels = bms_add_member(data_node_live_rels, i);
#endif
		}
		else
			ts_set_dummy_rel_pathlist(data_node_rel);

		set_cheapest(data_node_rel);

#ifdef TS_DEBUG
		if (ts_debug_optimizer_flags.show_rel)
			tsl_debug_log_rel_with_paths(root, data_node_rel, (UpperRelationKind *) NULL);
#endif
	}

	Assert(list_length(data_node_rels_list) > 0);

	/* Reset the pathlist since data node scans are preferred */
	hyper_rel->pathlist = NIL;

	/* Must keep partitioning info consistent with the append paths we create */
	hyper_rel->part_rels = data_node_rels;
	hyper_rel->nparts = ndata_node_rels;
#if PG15_GE
	hyper_rel->live_parts = data_node_live_rels;
#endif

	add_paths_to_append_rel(root, hyper_rel, data_node_rels_list);
	ts_cache_release(hcache);
}

/*
 * Creates CustomScanPath for the data node and adds to output_rel. No custom_path is added,
 * i.e., it is encapsulated by the CustomScanPath, so it doesn't inflate continuation of the
 * planning and will be planned locally on the data node.
 */
void
data_node_scan_create_upper_paths(PlannerInfo *root, UpperRelationKind stage, RelOptInfo *input_rel,
								  RelOptInfo *output_rel, void *extra)
{
	TimescaleDBPrivate *rel_private = input_rel->fdw_private;
	TsFdwRelInfo *fpinfo;

	if (rel_private == NULL || rel_private->fdw_relation_info == NULL)
		/* Not a rel we're interested in */
		return;

	fpinfo = fdw_relinfo_get(input_rel);

	/* Verify that this is a data node rel */
	if (NULL == fpinfo || fpinfo->type != TS_FDW_RELINFO_HYPERTABLE_DATA_NODE)
		return;

	fdw_create_upper_paths(fpinfo,
						   root,
						   stage,
						   input_rel,
						   output_rel,
						   extra,
						   data_node_scan_upper_path_create);
}

static CustomScanMethods data_node_scan_plan_methods = {
	.CustomName = "DataNodeScan",
	.CreateCustomScanState = data_node_scan_state_create,
};

typedef struct DataNodeScanPath
{
	CustomPath cpath;
} DataNodeScanPath;

static Plan *
data_node_scan_plan_create(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path, List *tlist,
						   List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);
	ScanInfo scaninfo;

	memset(&scaninfo, 0, sizeof(ScanInfo));

	fdw_scan_info_init(&scaninfo, root, rel, &best_path->path, clauses);

	cscan->methods = &data_node_scan_plan_methods;
	cscan->custom_plans = custom_plans;
	cscan->scan.plan.targetlist = tlist;
	cscan->scan.scanrelid = scaninfo.scan_relid;
	cscan->custom_scan_tlist = scaninfo.fdw_scan_tlist;
	cscan->scan.plan.qual = scaninfo.local_exprs;
	cscan->custom_exprs = list_make2(scaninfo.params_list, scaninfo.fdw_recheck_quals);

	/*
	 * If this is a join, and to make it valid to push down we had to assume
	 * that the current user is the same as some user explicitly named in the
	 * query, mark the finished plan as depending on the current user.
	 */
	if (rel->useridiscurrent)
		root->glob->dependsOnRole = true;

	/*
	 * If rel is a base relation, detect whether any system columns are
	 * requested from the rel.  (If rel is a join relation, rel->relid will be
	 * 0, but there can be no Var with relid 0 in the rel's targetlist or the
	 * restriction clauses, so we skip this in that case.  Note that any such
	 * columns in base relations that were joined are assumed to be contained
	 * in fdw_scan_tlist.)	This is a bit of a kluge and might go away
	 * someday, so we intentionally leave it out of the API presented to FDWs.
	 */

	scaninfo.systemcol = false;

	if (scaninfo.scan_relid > 0)
	{
		Bitmapset *attrs_used = NULL;
		ListCell *lc;
		int i;

		/*
		 * First, examine all the attributes needed for joins or final output.
		 * Note: we must look at rel's targetlist, not the attr_needed data,
		 * because attr_needed isn't computed for inheritance child rels.
		 */
		pull_varattnos((Node *) rel->reltarget->exprs, scaninfo.scan_relid, &attrs_used);

		/* Add all the attributes used by restriction clauses. */
		foreach (lc, rel->baserestrictinfo)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			pull_varattnos((Node *) rinfo->clause, scaninfo.scan_relid, &attrs_used);
		}

		/* Now, are any system columns requested from rel? */
		for (i = FirstLowInvalidHeapAttributeNumber + 1; i < 0; i++)
		{
			if (bms_is_member(i - FirstLowInvalidHeapAttributeNumber, attrs_used))
			{
				scaninfo.systemcol = true;
				break;
			}
		}

		bms_free(attrs_used);
	}

	/* Raise an error when system column is requsted, eg. tableoid */
	if (scaninfo.systemcol)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("system columns are not accessible on distributed hypertables with current "
						"settings"),
				 errhint("Set timescaledb.enable_per_data_node_queries=false to query system "
						 "columns.")));

	/* Should have determined the fetcher type by now. */
	Assert(ts_data_node_fetcher_scan_type != AutoFetcherType);

	cscan->custom_private = list_make3(scaninfo.fdw_private,
									   list_make1_int(scaninfo.systemcol),
									   makeInteger(ts_data_node_fetcher_scan_type));

	return &cscan->scan.plan;
}

static CustomPathMethods data_node_scan_path_methods = {
	.CustomName = DATA_NODE_SCAN_PATH_NAME,
	.PlanCustomPath = data_node_scan_plan_create,
};

static Path *
data_node_scan_path_create(PlannerInfo *root, RelOptInfo *rel, PathTarget *target, double rows,
						   Cost startup_cost, Cost total_cost, List *pathkeys,
						   Relids required_outer, Path *fdw_outerpath, List *private)
{
	DataNodeScanPath *scanpath = palloc0(sizeof(DataNodeScanPath));

	if (rel->lateral_relids && !bms_is_subset(rel->lateral_relids, required_outer))
		required_outer = bms_union(required_outer, rel->lateral_relids);

	if (!bms_is_empty(required_outer) && !IS_SIMPLE_REL(rel))
		elog(ERROR, "parameterized foreign joins are not supported yet");

	scanpath->cpath.path.type = T_CustomPath;
	scanpath->cpath.path.pathtype = T_CustomScan;
	scanpath->cpath.custom_paths = fdw_outerpath == NULL ? NIL : list_make1(fdw_outerpath);
	scanpath->cpath.methods = &data_node_scan_path_methods;
	scanpath->cpath.path.parent = rel;
	scanpath->cpath.path.pathtarget = target ? target : rel->reltarget;
	scanpath->cpath.path.param_info = get_baserel_parampathinfo(root, rel, required_outer);
	scanpath->cpath.path.parallel_aware = false;
	scanpath->cpath.path.parallel_safe = rel->consider_parallel;
	scanpath->cpath.path.parallel_workers = 0;
	scanpath->cpath.path.rows = rows;
	scanpath->cpath.path.startup_cost = startup_cost;
	scanpath->cpath.path.total_cost = total_cost;
	scanpath->cpath.path.pathkeys = pathkeys;

	return &scanpath->cpath.path;
}

static Path *
data_node_scan_upper_path_create(PlannerInfo *root, RelOptInfo *rel, PathTarget *target,
								 double rows, Cost startup_cost, Cost total_cost, List *pathkeys,
								 Path *fdw_outerpath, List *private)
{
	DataNodeScanPath *scanpath = palloc0(sizeof(DataNodeScanPath));

	/*
	 * Upper relations should never have any lateral references, since joining
	 * is complete.
	 */
	Assert(bms_is_empty(rel->lateral_relids));

	scanpath->cpath.path.type = T_CustomPath;
	scanpath->cpath.path.pathtype = T_CustomScan;
	scanpath->cpath.custom_paths = fdw_outerpath == NULL ? NIL : list_make1(fdw_outerpath);
	scanpath->cpath.methods = &data_node_scan_path_methods;
	scanpath->cpath.path.parent = rel;
	scanpath->cpath.path.pathtarget = target ? target : rel->reltarget;
	scanpath->cpath.path.param_info = NULL;
	scanpath->cpath.path.parallel_aware = false;
	scanpath->cpath.path.parallel_safe = rel->consider_parallel;
	scanpath->cpath.path.parallel_workers = 0;
	scanpath->cpath.path.rows = rows;
	scanpath->cpath.path.startup_cost = startup_cost;
	scanpath->cpath.path.total_cost = total_cost;
	scanpath->cpath.path.pathkeys = pathkeys;

	return &scanpath->cpath.path;
}
