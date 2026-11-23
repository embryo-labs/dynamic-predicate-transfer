#include "predicate_transfer/filter/bloom_filter.hpp"

#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include <random>
#include <cmath>
#include <iostream>

namespace duckdb {
namespace {
static uint32_t CeilPowerOfTwo(uint32_t n) {
	if (n <= 1) {
		return 1;
	}
	n--;
	n |= (n >> 1);
	n |= (n >> 2);
	n |= (n >> 4);
	n |= (n >> 8);
	n |= (n >> 16);
	return n + 1;
}

static Vector HashColumns(DataChunk &chunk, const vector<idx_t> &cols) {
	auto count = chunk.size();
	Vector hashes(LogicalType::HASH);
	hashes.Initialize(VectorDataInitialization::UNINITIALIZED, count);
	VectorOperations::Hash(chunk.data[cols[0]], hashes, count);
	for (size_t j = 1; j < cols.size(); j++) {
		VectorOperations::CombineHash(hashes, chunk.data[cols[j]], count);
	}

	// if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
	hashes.Flatten(count);
	// }

	return hashes;
}
} // namespace

RPTBloomFilter::RPTBloomFilter(ClientContext &context_p, const BloomFilterConfig &config, idx_t est_num_rows) {
	context = &context_p;
	buffer_manager = &BufferManager::GetBufferManager(*context);

	// Bound the multiplication in 64 bits and cap the result before rounding.
	constexpr uint64_t max_bits = uint64_t(MAX_NUM_SECTORS) << LOG_SECTOR_SIZE;
	const auto capped_rows = MinValue<uint64_t>(est_num_rows, max_bits);
	const auto needed_bits = MinValue<uint64_t>(capped_rows * config.bits_per_key, max_bits);
	const auto min_bits = MaxValue<uint64_t>(MIN_NUM_BITS, needed_bits);
	num_sectors = CeilPowerOfTwo(static_cast<uint32_t>(min_bits)) >> LOG_SECTOR_SIZE;
	num_sectors_log = static_cast<uint32_t>(std::log2(num_sectors));

	buf_ = buffer_manager->GetBufferAllocator().Allocate(64 + num_sectors * sizeof(uint32_t));
	// make sure blocks is a 64-byte aligned pointer, i.e., cache-line aligned
	blocks = reinterpret_cast<uint32_t *>((64ULL + reinterpret_cast<uint64_t>(buf_.get())) & ~63ULL);
	std::fill_n(blocks, num_sectors, 0);
}

int RPTBloomFilter::Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, SelectionVector &results,
                           size_t &result_count) const {
	int count = static_cast<int>(chunk.size());
	Vector hashes = HashColumns(chunk, bound_cols_applied);
	BloomFilterLookup(count, FlatVector::GetData<uint64_t>(hashes), blocks, results, result_count);
	return count;
}

int RPTBloomFilter::Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, Vector &results,
                           size_t &result_count) const {
	int count = static_cast<int>(chunk.size());
	Vector hashes = HashColumns(chunk, bound_cols_applied);
	BloomFilterLookup(count, FlatVector::GetData<uint64_t>(hashes), blocks, results, result_count);
	return count;
}

void RPTBloomFilter::Insert(DataChunk &chunk, const vector<idx_t> &bound_cols_built) {
	int count = static_cast<int>(chunk.size());
	Vector hashes = HashColumns(chunk, bound_cols_built);
	BloomFilterInsert(count, FlatVector::GetData<uint64_t>(hashes), blocks);
}
} // namespace duckdb
