/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */
#include <postgres.h>
#include <access/htup_details.h>
#include <catalog/pg_type.h>
#include <common/base64.h>
#include <funcapi.h>
#include <libpq/pqformat.h>
#include <lib/stringinfo.h>
#include <port/pg_bitutils.h>
#include <utils/builtins.h>
#include <utils/memutils.h>

#include "compression/compression.h"
#include "compression/gorilla.h"

#include "adts/bit_array.h"
#include "compression/simple8b_rle.h"
#include "compression/simple8b_rle_bitmap.h"
#include "float_utils.h"

/*
 * Gorilla compressed data is stored as
 *     uint16 compression_algorithm: id number for the compression scheme
 *     uint8 has_nulls: 1 if we store a NULLs bitmap after the data, otherwise 0
 *     uint8 bits_used_in_last_xor_bucket: number of bits used in the last bucket
 *     uint64 last_val: the last double stored, as bits
 *     simple8b_rle tag0: array of first tag bits (as in gorilla), also stores nelems
 *     simple8b_rle tag1: array of second tag bits (as in gorilla)
 *     BitArray leading_zeros: array of leading zeroes before the xor (as in gorilla)
 *     simple8b_rle num_bits_used: number of bits used for each xor (as in gorilla)
 *     BitArray xors: array xor values (as in gorilla)
 *     simple8b_rle nulls: 1 if the value is NULL, else 0
 */

typedef struct GorillaCompressed
{
	CompressedDataHeaderFields;
	uint8 has_nulls; /* we only use one bit for has_nulls, the rest can be reused */
	uint8 bits_used_in_last_xor_bucket;
	uint8 bits_used_in_last_leading_zeros_bucket;
	uint32 num_leading_zeroes_buckets;
	uint32 num_xor_buckets;
	uint64 last_value;
} GorillaCompressed;

#define BITS_PER_LEADING_ZEROS 6

/* expanded version of the compressed data */
typedef struct CompressedGorillaData
{
	const GorillaCompressed *header;
	Simple8bRleSerialized *tag0s;
	Simple8bRleSerialized *tag1s;
	BitArray leading_zeros;
	Simple8bRleSerialized *num_bits_used_per_xor;
	BitArray xors;
	Simple8bRleSerialized *nulls; /* NULL if no nulls */
} CompressedGorillaData;

static void
pg_attribute_unused() assertions(void)
{
	GorillaCompressed test_val = { .vl_len_ = { 0 } };
	/* make sure no padding bytes make it to disk */
	StaticAssertStmt(sizeof(GorillaCompressed) ==
						 sizeof(test_val.vl_len_) + sizeof(test_val.compression_algorithm) +
							 sizeof(test_val.has_nulls) +
							 sizeof(test_val.bits_used_in_last_xor_bucket) +
							 sizeof(test_val.bits_used_in_last_leading_zeros_bucket) +
							 sizeof(test_val.num_leading_zeroes_buckets) +
							 sizeof(test_val.num_xor_buckets) + sizeof(test_val.last_value),
					 "Gorilla wrong size");
	StaticAssertStmt(sizeof(GorillaCompressed) == 24, "Gorilla wrong size");
}

typedef struct GorillaCompressor
{
	// NOTE it is a small win to replace these next two with specialized RLE bitmaps
	Simple8bRleCompressor tag0s;
	Simple8bRleCompressor tag1s;
	BitArray leading_zeros;
	Simple8bRleCompressor bits_used_per_xor;
	BitArray xors;
	Simple8bRleCompressor nulls;

	uint64 prev_val;
	uint8 prev_leading_zeroes;
	uint8 prev_trailing_zeros;
	bool has_nulls;
} GorillaCompressor;

typedef struct ExtendedCompressor
{
	Compressor base;
	GorillaCompressor *internal;
} ExtendedCompressor;

typedef struct GorillaDecompressionIterator
{
	DecompressionIterator base;
	CompressedGorillaData gorilla_data;
	Simple8bRleBitmap tag0s;
	Simple8bRleBitmap tag1s;
	BitArrayIterator leading_zeros;
	Simple8bRleDecompressionIterator num_bits_used;
	BitArrayIterator xors;
	Simple8bRleBitmap nulls;
	uint64 prev_val;
	uint8 prev_leading_zeroes;
	uint8 prev_xor_bits_used;
	bool has_nulls;
} GorillaDecompressionIterator;

/********************
 ***  Compressor  ***
 ********************/

static void
gorilla_compressor_append_float(Compressor *compressor, Datum val)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	uint64 value = float_get_bits(DatumGetFloat4(val));
	if (extended->internal == NULL)
		extended->internal = gorilla_compressor_alloc();

	gorilla_compressor_append_value(extended->internal, value);
}

static void
gorilla_compressor_append_double(Compressor *compressor, Datum val)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	uint64 value = double_get_bits(DatumGetFloat8(val));
	if (extended->internal == NULL)
		extended->internal = gorilla_compressor_alloc();

	gorilla_compressor_append_value(extended->internal, value);
}

static void
gorilla_compressor_append_int16(Compressor *compressor, Datum val)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	if (extended->internal == NULL)
		extended->internal = gorilla_compressor_alloc();

	gorilla_compressor_append_value(extended->internal, (uint16) DatumGetInt16(val));
}

static void
gorilla_compressor_append_int32(Compressor *compressor, Datum val)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	if (extended->internal == NULL)
		extended->internal = gorilla_compressor_alloc();

	gorilla_compressor_append_value(extended->internal, (uint32) DatumGetInt32(val));
}

static void
gorilla_compressor_append_int64(Compressor *compressor, Datum val)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	if (extended->internal == NULL)
		extended->internal = gorilla_compressor_alloc();

	gorilla_compressor_append_value(extended->internal, DatumGetInt64(val));
}

static void
gorilla_compressor_append_null_value(Compressor *compressor)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	if (extended->internal == NULL)
		extended->internal = gorilla_compressor_alloc();

	gorilla_compressor_append_null(extended->internal);
}

static void *
gorilla_compressor_finish_and_reset(Compressor *compressor)
{
	ExtendedCompressor *extended = (ExtendedCompressor *) compressor;
	void *compressed = gorilla_compressor_finish(extended->internal);
	pfree(extended->internal);
	extended->internal = NULL;
	return compressed;
}

const Compressor gorilla_float_compressor = {
	.append_val = gorilla_compressor_append_float,
	.append_null = gorilla_compressor_append_null_value,
	.finish = gorilla_compressor_finish_and_reset,
};

const Compressor gorilla_double_compressor = {
	.append_val = gorilla_compressor_append_double,
	.append_null = gorilla_compressor_append_null_value,
	.finish = gorilla_compressor_finish_and_reset,
};
const Compressor gorilla_uint16_compressor = {
	.append_val = gorilla_compressor_append_int16,
	.append_null = gorilla_compressor_append_null_value,
	.finish = gorilla_compressor_finish_and_reset,
};
const Compressor gorilla_uint32_compressor = {
	.append_val = gorilla_compressor_append_int32,
	.append_null = gorilla_compressor_append_null_value,
	.finish = gorilla_compressor_finish_and_reset,
};
const Compressor gorilla_uint64_compressor = {
	.append_val = gorilla_compressor_append_int64,
	.append_null = gorilla_compressor_append_null_value,
	.finish = gorilla_compressor_finish_and_reset,
};

Compressor *
gorilla_compressor_for_type(Oid element_type)
{
	ExtendedCompressor *compressor = palloc(sizeof(*compressor));
	switch (element_type)
	{
		case FLOAT4OID:
			*compressor = (ExtendedCompressor){ .base = gorilla_float_compressor };
			return &compressor->base;
		case FLOAT8OID:
			*compressor = (ExtendedCompressor){ .base = gorilla_double_compressor };
			return &compressor->base;
		case INT2OID:
			*compressor = (ExtendedCompressor){ .base = gorilla_uint16_compressor };
			return &compressor->base;
		case INT4OID:
			*compressor = (ExtendedCompressor){ .base = gorilla_uint32_compressor };
			return &compressor->base;
		case INT8OID:
			*compressor = (ExtendedCompressor){ .base = gorilla_uint64_compressor };
			return &compressor->base;
		default:
			elog(ERROR,
				 "invalid type for Gorilla compression \"%s\"",
				 format_type_be(element_type));
	}
	pg_unreachable();
}

GorillaCompressor *
gorilla_compressor_alloc(void)
{
	GorillaCompressor *compressor = palloc(sizeof(*compressor));
	simple8brle_compressor_init(&compressor->tag0s);
	simple8brle_compressor_init(&compressor->tag1s);
	bit_array_init(&compressor->leading_zeros);
	simple8brle_compressor_init(&compressor->bits_used_per_xor);
	bit_array_init(&compressor->xors);
	simple8brle_compressor_init(&compressor->nulls);
	compressor->has_nulls = false;
	compressor->prev_leading_zeroes = 0;
	compressor->prev_trailing_zeros = 0;
	compressor->prev_val = 0;
	return compressor;
}

/* This function is used for testing only. */
Datum
tsl_gorilla_compressor_append(PG_FUNCTION_ARGS)
{
	MemoryContext old_context;
	MemoryContext agg_context;
	Compressor *compressor = (Compressor *) (PG_ARGISNULL(0) ? NULL : PG_GETARG_POINTER(0));

	if (!AggCheckCallContext(fcinfo, &agg_context))
	{
		/* cannot be called directly because of internal-type argument */
		elog(ERROR, "tsl_gorilla_compressor_append called in non-aggregate context");
	}

	old_context = MemoryContextSwitchTo(agg_context);

	if (compressor == NULL)
	{
		compressor = gorilla_compressor_for_type(get_fn_expr_argtype(fcinfo->flinfo, 1));
	}

	if (PG_ARGISNULL(1))
		compressor->append_null(compressor);
	else
	{
		compressor->append_val(compressor, PG_GETARG_DATUM(1));
	}

	MemoryContextSwitchTo(old_context);
	PG_RETURN_POINTER(compressor);
}

/* This function is used for testing only. */
Datum
tsl_gorilla_compressor_finish(PG_FUNCTION_ARGS)
{
	Compressor *compressor = (Compressor *) (PG_ARGISNULL(0) ? NULL : PG_GETARG_POINTER(0));

	if (compressor == NULL)
		PG_RETURN_NULL();

	void *compressed = compressor->finish(compressor);

	if (compressed == NULL)
		PG_RETURN_NULL();

	PG_RETURN_POINTER(compressed);
}

void
gorilla_compressor_append_null(GorillaCompressor *compressor)
{
	simple8brle_compressor_append(&compressor->nulls, 1);
	compressor->has_nulls = true;
}

void
gorilla_compressor_append_value(GorillaCompressor *compressor, uint64 val)
{
	bool has_values;
	uint64 xor = compressor->prev_val ^ val;
	simple8brle_compressor_append(&compressor->nulls, 0);

	/* for the first value we store the bitsize even if the xor is all zeroes,
	 * this ensures that the bits-per-xor isn't empty, and that we can calculate
	 * the remaining offsets correctly.
	 */
	has_values = !simple8brle_compressor_is_empty(&compressor->bits_used_per_xor);

	if (has_values && xor == 0)
		simple8brle_compressor_append(&compressor->tag0s, 0);
	else
	{
		/* leftmost/rightmost 1 is not well-defined when all the bits in the number
		 * are 0; the C implementations of these functions will ERROR, while the
		 * assembly versions may return any value. We special-case 0 to to use
		 * values for leading and trailing-zeroes that we know will work.
		 */
		int leading_zeros = xor != 0 ? 63 - pg_leftmost_one_pos64(xor) : 63;
		int trailing_zeros = xor != 0 ? pg_rightmost_one_pos64(xor) : 1;
		/*   This can easily get stuck with a bad value for trailing_zeroes, leading to a bad
		 * compressed size. We use a new trailing_zeroes if the delta is too large, but the
		 *   threshold (12) was picked in a completely unprincipled manner.
		 *   Needs benchmarking to determine an ideal threshold.
		 */
		bool reuse_bitsizes = has_values && leading_zeros >= compressor->prev_leading_zeroes &&
							  trailing_zeros >= compressor->prev_trailing_zeros &&
							  ((leading_zeros - compressor->prev_leading_zeroes) +
								   (trailing_zeros - compressor->prev_trailing_zeros) <=
							   12);
		uint8 num_bits_used;

		simple8brle_compressor_append(&compressor->tag0s, 1);
		simple8brle_compressor_append(&compressor->tag1s, reuse_bitsizes ? 0 : 1);
		if (!reuse_bitsizes)
		{
			compressor->prev_leading_zeroes = leading_zeros;
			compressor->prev_trailing_zeros = trailing_zeros;
			num_bits_used = 64 - (leading_zeros + trailing_zeros);

			bit_array_append(&compressor->leading_zeros, BITS_PER_LEADING_ZEROS, leading_zeros);
			simple8brle_compressor_append(&compressor->bits_used_per_xor, num_bits_used);
		}

		num_bits_used = 64 - (compressor->prev_leading_zeroes + compressor->prev_trailing_zeros);
		bit_array_append(&compressor->xors, num_bits_used, xor >> compressor->prev_trailing_zeros);
	}
	compressor->prev_val = val;
}

static GorillaCompressed *
compressed_gorilla_data_serialize(CompressedGorillaData *input)
{
	Size tags0s_size = simple8brle_serialized_total_size(input->tag0s);
	Size tags1s_size = simple8brle_serialized_total_size(input->tag1s);
	Size leading_zeros_size = bit_array_data_bytes_used(&input->leading_zeros);
	Size bits_used_per_xor_size = simple8brle_serialized_total_size(input->num_bits_used_per_xor);
	Size xors_size = bit_array_data_bytes_used(&input->xors);
	Size nulls_size = 0;

	Size compressed_size;
	char *data;
	GorillaCompressed *compressed;
	if (input->header->has_nulls)
		nulls_size = simple8brle_serialized_total_size(input->nulls);

	compressed_size = sizeof(GorillaCompressed) + tags0s_size + tags1s_size + leading_zeros_size +
					  bits_used_per_xor_size + xors_size;
	if (input->header->has_nulls)
		compressed_size += nulls_size;

	if (!AllocSizeIsValid(compressed_size))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("compressed size exceeds the maximum allowed (%d)", (int) MaxAllocSize)));

	data = palloc0(compressed_size);
	compressed = (GorillaCompressed *) data;
	SET_VARSIZE(&compressed->vl_len_, compressed_size);

	compressed->last_value = input->header->last_value;
	compressed->compression_algorithm = COMPRESSION_ALGORITHM_GORILLA;
	compressed->has_nulls = input->header->has_nulls;
	data += sizeof(GorillaCompressed);

	data = bytes_serialize_simple8b_and_advance(data, tags0s_size, input->tag0s);
	data = bytes_serialize_simple8b_and_advance(data, tags1s_size, input->tag1s);
	data = bytes_store_bit_array_and_advance(data,
											 leading_zeros_size,
											 &input->leading_zeros,
											 &compressed->num_leading_zeroes_buckets,
											 &compressed->bits_used_in_last_leading_zeros_bucket);
	data = bytes_serialize_simple8b_and_advance(data,
												bits_used_per_xor_size,
												input->num_bits_used_per_xor);
	data = bytes_store_bit_array_and_advance(data,
											 xors_size,
											 &input->xors,
											 &compressed->num_xor_buckets,
											 &compressed->bits_used_in_last_xor_bucket);

	if (input->header->has_nulls)
		data = bytes_serialize_simple8b_and_advance(data, nulls_size, input->nulls);
	return compressed;
}

void *
gorilla_compressor_finish(GorillaCompressor *compressor)
{
	GorillaCompressed header = {
		.compression_algorithm = COMPRESSION_ALGORITHM_GORILLA,
		.has_nulls = compressor->has_nulls ? 1 : 0,
		.last_value = compressor->prev_val,
	};
	CompressedGorillaData data = { .header = &header };
	data.tag0s = simple8brle_compressor_finish(&compressor->tag0s);
	if (data.tag0s == NULL)
		return NULL;

	data.tag1s = simple8brle_compressor_finish(&compressor->tag1s);
	Assert(data.tag1s != NULL);
	data.leading_zeros = compressor->leading_zeros;
	/* if all elements in the compressed are the same, there will be no xors,
	 * and thus bits_used_per_xor will be empty. Since we need to store the header
	 * to get the sizing right, we force at least one bits_used_per_xor to be created
	 * in append, above
	 */
	data.num_bits_used_per_xor = simple8brle_compressor_finish(&compressor->bits_used_per_xor);
	Assert(data.num_bits_used_per_xor != NULL);
	data.xors = compressor->xors;
	data.nulls = simple8brle_compressor_finish(&compressor->nulls);
	Assert(compressor->has_nulls || data.nulls != NULL);

	return compressed_gorilla_data_serialize(&data);
}

/*******************************
 ***  DecompressionIterator  ***
 *******************************/

inline static void
bytes_attach_bit_array_and_advance(BitArray *dst, StringInfo si, uint32 num_buckets,
								   uint8 bits_in_last_bucket)
{
	bit_array_wrap_internal(dst,
							num_buckets,
							bits_in_last_bucket,
							(uint64 *) (si->data + si->cursor));
	consumeCompressedData(si, bit_array_data_bytes_used(dst));
}

static void
compressed_gorilla_data_init_from_stringinfo(CompressedGorillaData *expanded, StringInfo si)
{
	expanded->header = (GorillaCompressed *) consumeCompressedData(si, sizeof(GorillaCompressed));

	if (expanded->header->compression_algorithm != COMPRESSION_ALGORITHM_GORILLA)
		elog(ERROR, "unknown compression algorithm");

	bool has_nulls = expanded->header->has_nulls == 1;

	expanded->tag0s = bytes_deserialize_simple8b_and_advance(si);
	expanded->tag1s = bytes_deserialize_simple8b_and_advance(si);

	bytes_attach_bit_array_and_advance(&expanded->leading_zeros,
									   si,
									   expanded->header->num_leading_zeroes_buckets,
									   expanded->header->bits_used_in_last_leading_zeros_bucket);

	expanded->num_bits_used_per_xor = bytes_deserialize_simple8b_and_advance(si);

	bytes_attach_bit_array_and_advance(&expanded->xors,
									   si,
									   expanded->header->num_xor_buckets,
									   expanded->header->bits_used_in_last_xor_bucket);

	if (has_nulls)
		expanded->nulls = bytes_deserialize_simple8b_and_advance(si);
	else
		expanded->nulls = NULL;
}

static void
compressed_gorilla_data_init_from_pointer(CompressedGorillaData *expanded,
										  const GorillaCompressed *compressed)
{
	StringInfoData si = { .data = (char *) compressed, .len = VARSIZE(compressed) };
	compressed_gorilla_data_init_from_stringinfo(expanded, &si);
}

static void
compressed_gorilla_data_init_from_datum(CompressedGorillaData *data, Datum gorilla_compressed)
{
	compressed_gorilla_data_init_from_pointer(data,
											  (GorillaCompressed *) PG_DETOAST_DATUM(
												  gorilla_compressed));
}

static void
gorilla_iterator_init_from_expanded_forward(GorillaDecompressionIterator *iterator,
											Oid element_type)
{
	iterator->base.compression_algorithm = COMPRESSION_ALGORITHM_GORILLA;
	iterator->base.forward = true;
	iterator->base.element_type = element_type;
	iterator->base.try_next = gorilla_decompression_iterator_try_next_forward;
	iterator->prev_val = 0;
	iterator->prev_leading_zeroes = 0;
	iterator->prev_xor_bits_used = 0;

	iterator->tag0s = simple8brle_bitmap_decompress(iterator->gorilla_data.tag0s);
	iterator->tag1s = simple8brle_bitmap_decompress(iterator->gorilla_data.tag1s);
	bit_array_iterator_init(&iterator->leading_zeros, &iterator->gorilla_data.leading_zeros);
	simple8brle_decompression_iterator_init_forward(&iterator->num_bits_used,
													iterator->gorilla_data.num_bits_used_per_xor);
	bit_array_iterator_init(&iterator->xors, &iterator->gorilla_data.xors);

	iterator->has_nulls = iterator->gorilla_data.nulls != NULL;
	if (iterator->has_nulls)
		iterator->nulls = simple8brle_bitmap_decompress(iterator->gorilla_data.nulls);
}

DecompressionIterator *
gorilla_decompression_iterator_from_datum_forward(Datum gorilla_compressed, Oid element_type)
{
	GorillaDecompressionIterator *iterator = palloc(sizeof(*iterator));
	compressed_gorilla_data_init_from_datum(&iterator->gorilla_data, gorilla_compressed);
	gorilla_iterator_init_from_expanded_forward(iterator, element_type);
	return &iterator->base;
}

static inline DecompressResult
convert_from_internal(DecompressResultInternal res_internal, Oid element_type)
{
	if (res_internal.is_done || res_internal.is_null)
	{
		return (DecompressResult){
			.is_done = res_internal.is_done,
			.is_null = res_internal.is_null,
		};
	}

	switch (element_type)
	{
		case FLOAT8OID:
			return (DecompressResult){
				.val = Float8GetDatum(bits_get_double(res_internal.val)),
			};
		case FLOAT4OID:
			return (DecompressResult){
				.val = Float4GetDatum(bits_get_float(res_internal.val)),
			};
		case INT8OID:
			return (DecompressResult){
				.val = Int64GetDatum(res_internal.val),
			};
		case INT4OID:
			return (DecompressResult){
				.val = Int32GetDatum(res_internal.val),
			};
		case INT2OID:
			return (DecompressResult){
				.val = Int16GetDatum(res_internal.val),
			};
		default:
			elog(ERROR, "invalid type requested from gorilla decompression");
	}
	pg_unreachable();
}

static DecompressResultInternal
gorilla_decompression_iterator_try_next_forward_internal(GorillaDecompressionIterator *iter)
{
	Simple8bRleDecompressResult tag0;
	Simple8bRleDecompressResult tag1;
	uint64 xor ;

	if (iter->has_nulls)
	{
		Simple8bRleDecompressResult null = simple8brle_bitmap_get_next(&iter->nulls);
		/* Could slightly improve performance here by not returning a tail of non-null bits */
		if (null.is_done)
			return (DecompressResultInternal){
				.is_done = true,
			};

		if (null.val != 0)
		{
			CheckCompressedData(null.val == 1);
			return (DecompressResultInternal){
				.is_null = true,
			};
		}
	}

	tag0 = simple8brle_bitmap_get_next(&iter->tag0s);
	/* if we don't have a null bitset, this will determine when we're done */
	if (tag0.is_done)
		return (DecompressResultInternal){
			.is_done = true,
		};

	if (tag0.val == 0)
		return (DecompressResultInternal){
			.val = iter->prev_val,
		};

	tag1 = simple8brle_bitmap_get_next(&iter->tag1s);
	CheckCompressedData(!tag1.is_done);

	if (tag1.val != 0)
	{
		Simple8bRleDecompressResult num_xor_bits;
		/* get new xor sizes */
		iter->prev_leading_zeroes =
			bit_array_iter_next(&iter->leading_zeros, BITS_PER_LEADING_ZEROS);
		CheckCompressedData(iter->prev_leading_zeroes <= 64);

		num_xor_bits = simple8brle_decompression_iterator_try_next_forward(&iter->num_bits_used);
		CheckCompressedData(!num_xor_bits.is_done);
		iter->prev_xor_bits_used = num_xor_bits.val;
		CheckCompressedData(iter->prev_xor_bits_used <= 64);

		CheckCompressedData(iter->prev_xor_bits_used + iter->prev_leading_zeroes <= 64);
	}

	xor = bit_array_iter_next(&iter->xors, iter->prev_xor_bits_used);
	if (iter->prev_leading_zeroes + iter->prev_xor_bits_used < 64)
		xor <<= 64 - (iter->prev_leading_zeroes + iter->prev_xor_bits_used);
	iter->prev_val ^= xor;

	return (DecompressResultInternal){
		.val = iter->prev_val,
	};
}

DecompressResult
gorilla_decompression_iterator_try_next_forward(DecompressionIterator *iter_base)
{
	Assert(iter_base->compression_algorithm == COMPRESSION_ALGORITHM_GORILLA && iter_base->forward);
	return convert_from_internal(gorilla_decompression_iterator_try_next_forward_internal(
									 (GorillaDecompressionIterator *) iter_base),
								 iter_base->element_type);
}

/****************************************
 *** reversed  DecompressionIterator  ***
 ****************************************/

/*
 * conceptually, the bits from the gorilla algorithm can be thought of like
 *      tag0:   1 1 1 1 1 1 1 1 1 1 1
 *      tag1:  1 0 0 0 0 1 0 0 0 0 1
 *     nbits: 0    4         5      3
 *       xor:   1 2 3 4 5 a b c d e Q
 * that is, tag1 represents the transition between one value in the number of
 * leading/used bits arrays, and thus can be transversed in any order, whenever
 * we see a `1`, we switch from using are current numbers to the "next" in
 * whichever iteration order we're following. When transversing in reverse order
 * there is a little subtlety in that we run out of lengths before we run out of
 * tag1 bits (there's an implicit leading `0`), but at that point we've run out
 * of values anyway, so it does not matter.
 */

DecompressionIterator *
gorilla_decompression_iterator_from_datum_reverse(Datum gorilla_compressed, Oid element_type)
{
	GorillaDecompressionIterator *iter = palloc(sizeof(*iter));
	Simple8bRleDecompressResult num_xor_bits;

	iter->base.compression_algorithm = COMPRESSION_ALGORITHM_GORILLA;
	iter->base.forward = false;
	iter->base.element_type = element_type;
	iter->base.try_next = gorilla_decompression_iterator_try_next_reverse;
	compressed_gorilla_data_init_from_datum(&iter->gorilla_data, gorilla_compressed);

	iter->tag0s = simple8brle_bitmap_decompress(iter->gorilla_data.tag0s);
	iter->tag1s = simple8brle_bitmap_decompress(iter->gorilla_data.tag1s);
	bit_array_iterator_init_rev(&iter->leading_zeros, &iter->gorilla_data.leading_zeros);
	simple8brle_decompression_iterator_init_reverse(&iter->num_bits_used,
													iter->gorilla_data.num_bits_used_per_xor);
	bit_array_iterator_init_rev(&iter->xors, &iter->gorilla_data.xors);

	iter->has_nulls = iter->gorilla_data.nulls != NULL;
	if (iter->has_nulls)
		iter->nulls = simple8brle_bitmap_decompress(iter->gorilla_data.nulls);

	/* we need to know how many bits are used, even if the last value didn't store them */
	iter->prev_leading_zeroes =
		bit_array_iter_next_rev(&iter->leading_zeros, BITS_PER_LEADING_ZEROS);
	num_xor_bits = simple8brle_decompression_iterator_try_next_reverse(&iter->num_bits_used);
	Assert(!num_xor_bits.is_done);
	iter->prev_xor_bits_used = num_xor_bits.val;
	iter->prev_val = iter->gorilla_data.header->last_value;
	return &iter->base;
}

static DecompressResultInternal
gorilla_decompression_iterator_try_next_reverse_internal(GorillaDecompressionIterator *iter)
{
	Simple8bRleDecompressResult tag0;
	Simple8bRleDecompressResult tag1;
	uint64 val;
	uint64 xor ;

	if (iter->has_nulls)
	{
		Simple8bRleDecompressResult null = simple8brle_bitmap_get_next_reverse(&iter->nulls);

		if (null.is_done)
			return (DecompressResultInternal){
				.is_done = true,
			};

		if (null.val != 0)
		{
			Assert(null.val == 1);
			return (DecompressResultInternal){
				.is_null = true,
			};
		}
	}

	val = iter->prev_val;

	tag0 = simple8brle_bitmap_get_next_reverse(&iter->tag0s);
	/* if we don't have a null bitset, this will determine when we're done */
	if (tag0.is_done)
		return (DecompressResultInternal){
			.is_done = true,
		};

	if (tag0.val == 0)
		return (DecompressResultInternal){
			.val = val,
		};

	xor = bit_array_iter_next_rev(&iter->xors, iter->prev_xor_bits_used);

	if (iter->prev_leading_zeroes + iter->prev_xor_bits_used < 64)
		xor <<= 64 - (iter->prev_leading_zeroes + iter->prev_xor_bits_used);
	iter->prev_val ^= xor;

	tag1 = simple8brle_bitmap_get_next_reverse(&iter->tag1s);

	if (tag1.val != 0)
	{
		/* get new xor sizes */
		Simple8bRleDecompressResult num_xor_bits =
			simple8brle_decompression_iterator_try_next_reverse(&iter->num_bits_used);
		/* there're an implicit leading 0 to num_xor_bits and prev_leading_zeroes,
		 */
		if (num_xor_bits.is_done)
		{
			iter->prev_xor_bits_used = 0;
			iter->prev_leading_zeroes = 0;
		}
		else
		{
			iter->prev_xor_bits_used = num_xor_bits.val;
			iter->prev_leading_zeroes =
				bit_array_iter_next_rev(&iter->leading_zeros, BITS_PER_LEADING_ZEROS);
		}
	}

	return (DecompressResultInternal){
		.val = val,
	};
}

DecompressResult
gorilla_decompression_iterator_try_next_reverse(DecompressionIterator *iter_base)
{
	Assert(iter_base->compression_algorithm == COMPRESSION_ALGORITHM_GORILLA &&
		   !iter_base->forward);
	return convert_from_internal(gorilla_decompression_iterator_try_next_reverse_internal(
									 (GorillaDecompressionIterator *) iter_base),
								 iter_base->element_type);
}

/*
 * Decompress packed 6bit values in lanes that contain a round number of both
 * packed and unpacked bytes -- 4 6-bit values are packed into 3 8-bit values.
 */
#define LANE_INPUTS 3
#define LANE_OUTPUTS 4

pg_attribute_always_inline static void
unpack_leading_zeros_array(BitArray *bitarray, uint8 *restrict dest)
{
	Assert(BITS_PER_LEADING_ZEROS == 6);

	const int n_packed = bitarray->buckets.num_elements * sizeof(uint64);
	const uint8 *restrict packed = ((uint8 *) bitarray->buckets.data);

	/*
	 * We have two more arrays after leading zeros, so we don't care if the
	 * reads of final bytes run into them and we unpack some nonsense. This
	 * means we can always work in full lanes.
	 */
	const int n_lanes = (n_packed + LANE_INPUTS - 1) / LANE_INPUTS;
	for (int lane = 0; lane < n_lanes; lane++)
	{
		uint8 *restrict lane_dest = &dest[lane * LANE_OUTPUTS];
		const uint8 *restrict lane_src = &packed[lane * LANE_INPUTS];
		for (int output_in_lane = 0; output_in_lane < LANE_OUTPUTS; output_in_lane++)
		{
			const int startbit_abs = output_in_lane * 6;
			const int startbit_rel = startbit_abs % 8;
			const int offs = 8 - startbit_rel;

			const uint8 this_input = lane_src[startbit_abs / 8];
			const uint8 next_input = lane_src[(startbit_abs + 6 - 1) / 8];

			uint8 output = this_input >> startbit_rel;
			output |= ((uint64) next_input) << offs;
			output &= (1ull << 6) - 1ull;

			lane_dest[output_in_lane] = output;
		}
	}
}

#undef LANE_INPUTS
#undef LANE_OUTPUTS

// typedef struct
//{
//	uint64 *restrict data;
//	uint64 start_bit_absolute;
//	uint64 this_word;
// } SimpleBitArrayIterator;
//
// pg_attribute_always_inline static uint64
// simple_bit_array_next(SimpleBitArrayIterator *iter, uint8 bits)
//{
//	Assert(bits <= 64);
//
//	if (bits == 0)
//	{
//		return 0;
//	}
//
//	const uint64 start_bit_absolute = iter->start_bit_absolute;
//	const uint64 end_bit_absolute = start_bit_absolute + bits;
//	const uint64 start_word_index = start_bit_absolute / 64;
//	const uint64 end_word_index = (end_bit_absolute - 1) / 64;
//	const uint64 start_bit_in_word = start_bit_absolute % 64;
//	// const uint64 end_bit_in_word = end_bit_absolute % 64;
//
//	iter->start_bit_absolute = end_bit_absolute;
//
//	const uint64 lower_mask = (-1ull) >> (64 - bits);
//
//	const uint64 this_word = iter->data[start_word_index];
//	if (end_word_index == start_word_index)
//	{
//		return (this_word >> start_bit_in_word) & lower_mask;
//	}
//
//	/*
//	 * Our bits span the next word as well. offs is the first bit in the output
//	 * that is to the next input word.
//	 */
//	const uint8 offs = 64 - start_bit_in_word;
//	Assert(offs > 0);
//	Assert(offs < 64);
//	const uint64 next_word = iter->data[end_word_index];
//	return ((this_word >> start_bit_in_word) | (next_word << offs)) & lower_mask;
// }

// pg_attribute_always_inline static uint64
// simple_bit_array_next(SimpleBitArrayIterator *iter, uint8 bits)
// {
// 	/*
// 	 * Bits can be 0 if the first number after tags (1, 1) is zero.
// 	 */
// 	Assert(bits <= 64);
// 	if (bits == 0 || bits > 64)
// 	{
// 		return 0;
// 	}
//
// 	const uint64 start_bit_absolute = iter->start_bit_absolute;
// 	const uint64 end_bit_absolute = start_bit_absolute + bits;
// 	const uint64 start_bit_in_word = start_bit_absolute % 64;
// 	const uint64 end_bit_in_word = end_bit_absolute % 64;
//
// 	uint64 output = iter->this_word;
// 	if (start_bit_in_word + bits < 64)
// 	{
// 		iter->this_word >>= bits;
// 	}
// 	else
// 	{
// 		/*
// 		 * This is the bit in the output, where the bits from the next input word go.
// 		 */
// 		const uint8 offs = 64 - start_bit_in_word;
// 		Assert(offs > 0);
// 		Assert(offs <= 64);
// 		/*
// 		 * If we think we have to load the next word, at least the bit before
// 		 * the last should be in the next word.
// 		 */
// 		Assert(1 + start_bit_absolute / 64 == end_bit_absolute / 64);
//
// 		const uint64 next_word = iter->data[end_bit_absolute / 64];
//
// 		if (offs < 64)
// 		{
// 			/*
// 			 * Can be 64 when we are asked to get an entire word starting on word
// 			 * boundary. We can't do this shift in that case.
// 			 */
// 			output |= (next_word << offs);
// 		}
//
// 		iter->this_word = next_word >> end_bit_in_word;
// 	}
//
// 	iter->start_bit_absolute = end_bit_absolute;
//
// 	const uint64 lower_mask = (-1ull) >> (64 - bits);
// 	return output & lower_mask;
// }

/* Bulk gorilla decompression, specialized for supported data types. */

#define ELEMENT_TYPE uint32
#include "gorilla_impl.c"
#undef ELEMENT_TYPE

#define ELEMENT_TYPE uint64
#include "gorilla_impl.c"
#undef ELEMENT_TYPE

ArrowArray *
gorilla_decompress_all_forward_direction(Datum datum, Oid element_type)
{
	CompressedGorillaData gorilla_data;
	compressed_gorilla_data_init_from_datum(&gorilla_data, datum);

	switch (element_type)
	{
		case FLOAT8OID:
			return gorilla_decompress_all_uint64(&gorilla_data);
		case FLOAT4OID:
			return gorilla_decompress_all_uint32(&gorilla_data);
		default:
			elog(ERROR, "type oid %d is not supported for gorilla decompression", element_type);
			return NULL;
	}
}

/*************
 ***  I/O  ***
 **************/

void
gorilla_compressed_send(CompressedDataHeader *header, StringInfo buf)
{
	CompressedGorillaData data;
	const GorillaCompressed *compressed = (GorillaCompressed *) header;
	Assert(header->compression_algorithm == COMPRESSION_ALGORITHM_GORILLA);

	compressed_gorilla_data_init_from_pointer(&data, compressed);
	pq_sendbyte(buf, data.header->has_nulls);
	pq_sendint64(buf, data.header->last_value);
	simple8brle_serialized_send(buf, data.tag0s);
	simple8brle_serialized_send(buf, data.tag1s);
	bit_array_send(buf, &data.leading_zeros);
	simple8brle_serialized_send(buf, data.num_bits_used_per_xor);
	bit_array_send(buf, &data.xors);
	if (data.header->has_nulls)
		simple8brle_serialized_send(buf, data.nulls);
}

Datum
gorilla_compressed_recv(StringInfo buf)
{
	GorillaCompressed header = { .vl_len_ = { 0 } };
	CompressedGorillaData data = {
		.header = &header,
	};

	header.has_nulls = pq_getmsgbyte(buf);
	if (header.has_nulls != 0 && header.has_nulls != 1)
	{
		elog(ERROR, "invalid recv in gorilla: bad bool");
	}

	header.last_value = pq_getmsgint64(buf);
	data.tag0s = simple8brle_serialized_recv(buf);
	data.tag1s = simple8brle_serialized_recv(buf);
	data.leading_zeros = bit_array_recv(buf);
	data.num_bits_used_per_xor = simple8brle_serialized_recv(buf);
	data.xors = bit_array_recv(buf);

	if (header.has_nulls)
		data.nulls = simple8brle_serialized_recv(buf);

	PG_RETURN_POINTER(compressed_gorilla_data_serialize(&data));
}
