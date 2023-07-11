/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#pragma once

#include "compression/compression.h"
#include "nodes/decompress_chunk/exec.h"

typedef struct ArrowArray ArrowArray;

typedef struct CompressedColumnValues
{
	/* For row-by-row decompression. */
	DecompressionIterator *iterator;

	/*
	 * For bulk decompression and vectorized filters, mutually exclusive
	 * with the above.
	 */
	ArrowArray *arrow;

	/*
	 * These are the arrow buffers cached here to reduce the amount of
	 * indirections (we have about three there, so it matters).
	 */
	const void *arrow_validity;
	const void *arrow_values;

	/*
	 * The following fields are copied here for better data locality.
	 */
	AttrNumber output_attno;
	int8 value_bytes;
} CompressedColumnValues;

/*
 * All the needed information to decompress a batch
 */
typedef struct DecompressBatchState
{
	bool initialized;
	TupleTableSlot *decompressed_scan_slot; /* A slot for the decompressed data */
	TupleTableSlot *compressed_slot;		/* A slot for compressed data */
	int total_batch_rows;
	int current_batch_row;
	MemoryContext per_batch_context;
	uint64 *vector_qual_result;

	CompressedColumnValues compressed_columns[FLEXIBLE_ARRAY_MEMBER];
} DecompressBatchState;

extern void compressed_batch_init(DecompressChunkState *chunk_state,
								  DecompressBatchState *batch_state);

extern void compressed_batch_set_compressed_tuple(DecompressChunkState *chunk_state,
												  DecompressBatchState *batch_state,
												  TupleTableSlot *subslot);

extern void compressed_batch_advance(DecompressChunkState *chunk_state,
									 DecompressBatchState *batch_state);
