/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <miscadmin.h>
#include <access/sysattr.h>
#include <executor/executor.h>
#include <nodes/bitmapset.h>
#include <nodes/makefuncs.h>
#include <nodes/nodeFuncs.h>
#include <parser/parsetree.h>
#include <rewrite/rewriteManip.h>
#include <utils/builtins.h>
#include <utils/date.h>
#include <utils/datum.h>
#include <utils/memutils.h>
#include <utils/typcache.h>

#include "compat/compat.h"
#include "compression/array.h"
#include "compression/compression.h"
#include "nodes/decompress_chunk/decompress_chunk.h"
#include "nodes/decompress_chunk/exec.h"
#include "nodes/decompress_chunk/planner.h"
#include "ts_catalog/hypertable_compression.h"

typedef enum DecompressChunkColumnType
{
	SEGMENTBY_COLUMN,
	COMPRESSED_COLUMN,
	COUNT_COLUMN,
	SEQUENCE_NUM_COLUMN,
} DecompressChunkColumnType;

typedef struct DecompressChunkColumnState
{
	DecompressChunkColumnType type;
	Oid typid;

	/*
	 * Attno of the decompressed column in the output of DecompressChunk node.
	 * Negative values are special columns that do not have a representation in
	 * the uncompressed chunk, but are still used for decompression. They should
	 * have the respective `type` field.
	 */
	AttrNumber output_attno;

	/*
	 * Attno of the compressed column in the input compressed chunk scan.
	 */
	AttrNumber compressed_scan_attno;

	union
	{
		struct
		{
			Datum value;
			bool isnull;
			int count;
		} segmentby;

		struct
		{
			/* For row-by-row decompression. */
			DecompressionIterator *iterator;

			/* Exclusive to the above, if we decompressed the entire batch. */
			Datum *datums;
			bool *nulls;
		} compressed;
	};
} DecompressChunkColumnState;

typedef struct DecompressChunkState
{
	CustomScanState csstate;
	List *decompression_map;
	List *is_segmentby_column;
	int num_columns;
	DecompressChunkColumnState *columns;

	bool initialized;
	bool reverse;
	int hypertable_id;
	Oid chunk_relid;
	int total_batch_rows;
	int current_batch_row;
	MemoryContext per_batch_context;
	MemoryContext arrow_context;
} DecompressChunkState;

static TupleTableSlot *decompress_chunk_exec(CustomScanState *node);
static void decompress_chunk_begin(CustomScanState *node, EState *estate, int eflags);
static void decompress_chunk_end(CustomScanState *node);
static void decompress_chunk_rescan(CustomScanState *node);
static TupleTableSlot *decompress_chunk_create_tuple(DecompressChunkState *state);

static CustomExecMethods decompress_chunk_state_methods = {
	.BeginCustomScan = decompress_chunk_begin,
	.ExecCustomScan = decompress_chunk_exec,
	.EndCustomScan = decompress_chunk_end,
	.ReScanCustomScan = decompress_chunk_rescan,
};

Node *
decompress_chunk_state_create(CustomScan *cscan)
{
	DecompressChunkState *state;
	List *settings;

	state = (DecompressChunkState *) newNode(sizeof(DecompressChunkState), T_CustomScanState);

	state->csstate.methods = &decompress_chunk_state_methods;

	settings = linitial(cscan->custom_private);
	state->hypertable_id = linitial_int(settings);
	state->chunk_relid = lsecond_int(settings);
	state->reverse = lthird_int(settings);
	state->decompression_map = lsecond(cscan->custom_private);
	state->is_segmentby_column = lthird(cscan->custom_private);

	return (Node *) state;
}

/*
 * initialize column state
 *
 * the column state indexes are based on the index
 * of the columns of the uncompressed chunk because
 * that is the tuple layout we are creating
 */
static void
initialize_column_state(DecompressChunkState *state)
{
	ScanState *ss = (ScanState *) state;
	TupleDesc desc = ss->ss_ScanTupleSlot->tts_tupleDescriptor;

	if (list_length(state->decompression_map) == 0)
	{
		elog(ERROR, "no columns specified to decompress");
	}

	state->columns =
		palloc0(list_length(state->decompression_map) * sizeof(DecompressChunkColumnState));

	AttrNumber next_compressed_scan_attno = 0;
	state->num_columns = 0;
	ListCell *dest_cell;
	ListCell *is_segmentby_cell;
	forboth (dest_cell, state->decompression_map, is_segmentby_cell, state->is_segmentby_column)
	{
		next_compressed_scan_attno++;

		AttrNumber output_attno = lfirst_int(dest_cell);
		if (output_attno == 0)
		{
			/* We are asked not to decompress this column, skip it. */
			continue;
		}

		DecompressChunkColumnState *column = &state->columns[state->num_columns];
		state->num_columns++;

		column->output_attno = output_attno;
		column->compressed_scan_attno = next_compressed_scan_attno;

		if (output_attno > 0)
		{
			/* normal column that is also present in uncompressed chunk */
			Form_pg_attribute attribute =
				TupleDescAttr(desc, AttrNumberGetAttrOffset(output_attno));

			column->typid = attribute->atttypid;

			if (lfirst_int(is_segmentby_cell))
				column->type = SEGMENTBY_COLUMN;
			else
				column->type = COMPRESSED_COLUMN;
		}
		else
		{
			/* metadata columns */
			switch (column->output_attno)
			{
				case DECOMPRESS_CHUNK_COUNT_ID:
					column->type = COUNT_COLUMN;
					break;
				case DECOMPRESS_CHUNK_SEQUENCE_NUM_ID:
					column->type = SEQUENCE_NUM_COLUMN;
					break;
				default:
					elog(ERROR, "Invalid column attno \"%d\"", column->output_attno);
					break;
			}
		}
	}
}

typedef struct ConstifyTableOidContext
{
	Index chunk_index;
	Oid chunk_relid;
	bool made_changes;
} ConstifyTableOidContext;

static Node *
constify_tableoid_walker(Node *node, ConstifyTableOidContext *ctx)
{
	if (node == NULL)
		return NULL;

	if (IsA(node, Var))
	{
		Var *var = castNode(Var, node);

		if ((Index) var->varno != ctx->chunk_index)
			return node;

		if (var->varattno == TableOidAttributeNumber)
		{
			ctx->made_changes = true;
			return (
				Node *) makeConst(OIDOID, -1, InvalidOid, 4, (Datum) ctx->chunk_relid, false, true);
		}

		/*
		 * we doublecheck system columns here because projection will
		 * segfault if any system columns get through
		 */
		if (var->varattno < SelfItemPointerAttributeNumber)
			elog(ERROR, "transparent decompression only supports tableoid system column");

		return node;
	}

	return expression_tree_mutator(node, constify_tableoid_walker, (void *) ctx);
}

static List *
constify_tableoid(List *node, Index chunk_index, Oid chunk_relid)
{
	ConstifyTableOidContext ctx = {
		.chunk_index = chunk_index,
		.chunk_relid = chunk_relid,
		.made_changes = false,
	};

	List *result = (List *) constify_tableoid_walker((Node *) node, &ctx);
	if (ctx.made_changes)
	{
		return result;
	}

	return node;
}

/*
 * Complete initialization of the supplied CustomScanState.
 *
 * Standard fields have been initialized by ExecInitCustomScan,
 * but any private fields should be initialized here.
 */
static void
decompress_chunk_begin(CustomScanState *node, EState *estate, int eflags)
{
	DecompressChunkState *state = (DecompressChunkState *) node;
	CustomScan *cscan = castNode(CustomScan, node->ss.ps.plan);
	Plan *compressed_scan = linitial(cscan->custom_plans);
	Assert(list_length(cscan->custom_plans) == 1);

	PlanState *ps = &node->ss.ps;
	if (ps->ps_ProjInfo)
	{
		/*
		 * if we are projecting we need to constify tableoid references here
		 * because decompressed tuple are virtual tuples and don't have
		 * system columns.
		 *
		 * We do the constify in executor because even after plan creation
		 * our targetlist might still get modified by parent nodes pushing
		 * down targetlist.
		 */
		List *tlist = ps->plan->targetlist;
		List *modified_tlist = constify_tableoid(tlist, cscan->scan.scanrelid, state->chunk_relid);

		if (modified_tlist != tlist)
		{
			ps->ps_ProjInfo =
				ExecBuildProjectionInfo(modified_tlist,
										ps->ps_ExprContext,
										ps->ps_ResultTupleSlot,
										ps,
										node->ss.ss_ScanTupleSlot->tts_tupleDescriptor);
		}
	}

	initialize_column_state(state);

	node->custom_ps = lappend(node->custom_ps, ExecInitNode(compressed_scan, estate, eflags));

	state->per_batch_context = AllocSetContextCreate(CurrentMemoryContext,
													 "DecompressChunk per_batch",
													 /* minContextSize = */ 0,
													 /* initBlockSize = */ 64 * 1024,
													 /* maxBlockSize = */ 64 * 1024);

	state->arrow_context = AllocSetContextCreate(CurrentMemoryContext,
												 "DecompressChunk Arrow arrays",
												 /* minContextSize = */ 0,
												 /* initBlockSize = */ 64 * 1024,
												 /* maxBlockSize = */ 64 * 1024);
}

static void
initialize_batch(DecompressChunkState *state, TupleTableSlot *compressed_slot,
				 TupleTableSlot *decompressed_slot)
{
	Datum value;
	bool isnull;
	int i;
	MemoryContext old_context = MemoryContextSwitchTo(state->per_batch_context);
	MemoryContextReset(state->per_batch_context);

	state->total_batch_rows = 0;
	state->current_batch_row = 0;

	for (i = 0; i < state->num_columns; i++)
	{
		DecompressChunkColumnState *column = &state->columns[i];

		switch (column->type)
		{
			case COMPRESSED_COLUMN:
			{
				column->compressed.datums = NULL;
				column->compressed.iterator = NULL;

				value = slot_getattr(compressed_slot, column->compressed_scan_attno, &isnull);

				if (isnull)
				{
					column->compressed.iterator = NULL;
					AttrNumber attr = AttrNumberGetAttrOffset(column->output_attno);
					decompressed_slot->tts_values[attr] =
						getmissingattr(decompressed_slot->tts_tupleDescriptor,
									   attr + 1,
									   &decompressed_slot->tts_isnull[attr]);
					break;
				}

				/* Decompress the entire batch if it is supported. */
				CompressedDataHeader *header = (CompressedDataHeader *) PG_DETOAST_DATUM(value);

				MemoryContext context_before_decompression =
					MemoryContextSwitchTo(state->arrow_context);
				ArrowArray *arrow = tsl_try_decompress_all(header->compression_algorithm,
														   PointerGetDatum(header),
														   column->typid);
				MemoryContextSwitchTo(context_before_decompression);

				if (arrow)
				{
					/*
					 * Currently we're going to convert the Arrow arrays to postgres
					 * Data right away, but in the future we will perform vectorized
					 * operations on Arrow arrays directly before that.
					 */
					const int n = arrow->length;
#define INNER_LOOP_SIZE 8

					const int n_padded =
						((n + INNER_LOOP_SIZE - 1) / INNER_LOOP_SIZE) * (INNER_LOOP_SIZE);
					Assert(n > 0);

					const int arrow_bitmap_elements = (n + 64 - 1) / 64;
					const int n_nulls_padded = arrow_bitmap_elements * 64;

					if (state->total_batch_rows == 0)
					{
						state->total_batch_rows = n;
					}
					else if (state->total_batch_rows != n)
					{
						elog(ERROR, "compressed column out of sync with batch counter");
					}

					/* Convert Arrow to Data/nulls arrays. */
					const uint64 *restrict validity_bitmap = arrow->buffers[0];
					const void *restrict arrow_values = arrow->buffers[1];
					Datum *restrict datums = palloc(sizeof(Datum) * n_padded);
					bool *restrict nulls = palloc(sizeof(bool) * n_nulls_padded);

#define CONVERSION_LOOP(OID, CTYPE, CONVERSION)                                                    \
	case OID:                                                                                      \
		for (int outer = 0; outer < n_padded; outer += INNER_LOOP_SIZE)                            \
		{                                                                                          \
			for (int inner = 0; inner < INNER_LOOP_SIZE; inner++)                                  \
			{                                                                                      \
				const int output_row = outer + inner;                                              \
				const int input_row = reverse ? n - output_row - 1 : output_row;                   \
				datums[output_row] = CONVERSION(((CTYPE *) arrow_values)[input_row]);              \
			}                                                                                      \
		}                                                                                          \
		break

#define FOR_ONE_DIRECTION                                                                          \
	switch (column->typid)                                                                         \
	{                                                                                              \
		CONVERSION_LOOP(INT2OID, int16, Int16GetDatum);                                            \
		CONVERSION_LOOP(INT4OID, int32, Int32GetDatum);                                            \
		CONVERSION_LOOP(INT8OID, int64, Int64GetDatum);                                            \
		CONVERSION_LOOP(FLOAT4OID, float4, Float4GetDatum);                                        \
		CONVERSION_LOOP(FLOAT8OID, float8, Float8GetDatum);                                        \
		CONVERSION_LOOP(DATEOID, int32, DateADTGetDatum);                                          \
		CONVERSION_LOOP(TIMESTAMPOID, int64, TimestampGetDatum);                                   \
		CONVERSION_LOOP(TIMESTAMPTZOID, int64, TimestampTzGetDatum);                               \
	}                                                                                              \
	for (int outer = 0; outer < arrow_bitmap_elements; outer++)                                    \
	{                                                                                              \
		const uint64_t element = validity_bitmap[outer];                                           \
		for (int inner = 0; inner < 64; inner++)                                                   \
		{                                                                                          \
			const int input_row = outer * 64 + inner;                                              \
			const int output_row = reverse ? n - input_row - 1 : input_row;                        \
			nulls[output_row] = ((element >> inner) & 1) ? false : true;                           \
		}                                                                                          \
	}

					const bool reverse = state->reverse;
					if (reverse)
					{
						FOR_ONE_DIRECTION
					}
					else
					{
						FOR_ONE_DIRECTION
					}

#undef CONVERSION_LOOP
#undef FOR_ONE_DIRECTION

#undef INNER_LOOP_SIZE

					MemoryContextReset(state->arrow_context);

					column->compressed.iterator = NULL;
					column->compressed.datums = datums;
					column->compressed.nulls = nulls;

					break;
				}

				/* As a fallback, decompress row-by-row. */
				column->compressed.iterator =
					tsl_get_decompression_iterator_init(header->compression_algorithm,
														state->reverse)(PointerGetDatum(header),
																		column->typid);

				break;
			}
			case SEGMENTBY_COLUMN:
			{
				AttrNumber attr = AttrNumberGetAttrOffset(column->output_attno);
				decompressed_slot->tts_values[attr] =
					slot_getattr(compressed_slot,
								 column->compressed_scan_attno,
								 &decompressed_slot->tts_isnull[attr]);
				break;
			}
			case COUNT_COLUMN:
				value = slot_getattr(compressed_slot, column->compressed_scan_attno, &isnull);
				int count_value = DatumGetInt32(value);
				Assert(count_value > 0);
				if (state->total_batch_rows == 0)
				{
					state->total_batch_rows = count_value;
				}
				else if (state->total_batch_rows != count_value)
				{
					elog(ERROR, "compressed column out of sync with batch counter");
				}
				/* count column should never be NULL */
				Assert(!isnull);
				break;
			case SEQUENCE_NUM_COLUMN:
				/*
				 * nothing to do here for sequence number
				 * we only needed this for sorting in node below
				 */
				break;
		}
	}
	state->initialized = true;
	MemoryContextSwitchTo(old_context);

	// fprintf(stderr, "initialized new batch with %d rows\n", state->total_batch_rows);
}

static TupleTableSlot *
decompress_chunk_exec(CustomScanState *node)
{
	DecompressChunkState *state = (DecompressChunkState *) node;
	ExprContext *econtext = node->ss.ps.ps_ExprContext;

	if (node->custom_ps == NIL)
		return NULL;

	while (true)
	{
		TupleTableSlot *decompressed_slot = decompress_chunk_create_tuple(state);

		if (TupIsNull(decompressed_slot))
			return NULL;

		econtext->ecxt_scantuple = decompressed_slot;

		if (node->ss.ps.qual && !ExecQual(node->ss.ps.qual, econtext))
		{
			InstrCountFiltered1(node, 1);
			ExecClearTuple(decompressed_slot);
			continue;
		}

		if (!node->ss.ps.ps_ProjInfo)
			return decompressed_slot;

		return ExecProject(node->ss.ps.ps_ProjInfo);
	}
}

static void
decompress_chunk_rescan(CustomScanState *node)
{
	((DecompressChunkState *) node)->initialized = false;
	ExecReScan(linitial(node->custom_ps));
}

static void
decompress_chunk_end(CustomScanState *node)
{
	MemoryContextReset(((DecompressChunkState *) node)->per_batch_context);
	ExecEndNode(linitial(node->custom_ps));
}

/*
 * Create generated tuple according to column state
 */
static TupleTableSlot *
decompress_chunk_create_tuple(DecompressChunkState *state)
{
	TupleTableSlot *slot = state->csstate.ss.ss_ScanTupleSlot;

	while (true)
	{
		if (state->initialized && state->current_batch_row >= state->total_batch_rows)
		{
			/*
			 * Reached end of batch. Check that the columns that we're decompressing
			 * row-by-row have also ended.
			 */
			state->initialized = false;
			for (int i = 0; i < state->num_columns; i++)
			{
				DecompressChunkColumnState *column = &state->columns[i];
				if (column->type == COMPRESSED_COLUMN && column->compressed.iterator)
				{
					DecompressResult result =
						column->compressed.iterator->try_next(column->compressed.iterator);
					if (!result.is_done)
					{
						elog(ERROR, "compressed column out of sync with batch counter");
					}
				}
			}
		}

		if (!state->initialized)
		{
			ExecStoreAllNullTuple(decompressed_slot);

			/*
			 * Reset expression memory context to clean out any cruft from
			 * previous batch. Our batches are 1000 rows max, and this memory
			 * context is used by ExecProject and ExecQual, which shouldn't
			 * leak too much. So we only do this per batch and not per tuple to
			 * save some CPU.
			 */
			ExprContext *econtext = state->csstate.ss.ps.ps_ExprContext;
			ResetExprContext(econtext);

			TupleTableSlot *subslot = ExecProcNode(linitial(state->csstate.custom_ps));

			if (TupIsNull(subslot))
				return NULL;

			initialize_batch(state, subslot, slot);
		}

		Assert(state->initialized);
		Assert(state->total_batch_rows > 0);
		Assert(state->current_batch_row < state->total_batch_rows);

		for (int i = 0; i < state->num_columns; i++)
		{
			DecompressChunkColumnState *column = &state->columns[i];
			if (column->type != COMPRESSED_COLUMN)
			{
				continue;
			}

			const AttrNumber attr = AttrNumberGetAttrOffset(column->output_attno);

			if (column->compressed.datums)
			{
				Assert(column->compressed.iterator == NULL);
				Assert(column->compressed.nulls != NULL);

				slot->tts_isnull[attr] = column->compressed.nulls[state->current_batch_row];
				slot->tts_values[attr] = column->compressed.datums[state->current_batch_row];
			}
			else if (column->compressed.iterator != NULL)
			{
				DecompressResult result =
					column->compressed.iterator->try_next(column->compressed.iterator);

				if (result.is_done)
				{
					elog(ERROR, "compressed column out of sync with batch counter");
				}

				slot->tts_isnull[attr] = result.is_null;
				slot->tts_values[attr] = result.val;
			}
		}

		/*
		 * It's a virtual tuple slot, so no point in clearing/storing it
		 * per each row, we can just update the values in-place. This saves
		 * some CPU. We have to store it after ExecQual fails or after a new
		 * batch.
		 */
		if (TTS_EMPTY(slot))
		{
			ExecStoreVirtualTuple(slot);
		}

		state->current_batch_row++;
		return slot;
	}
}
