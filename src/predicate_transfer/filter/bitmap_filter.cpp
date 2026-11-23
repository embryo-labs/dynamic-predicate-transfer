#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "predicate_transfer/filter/bitmap_filter.hpp"

#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <random>
#include <cmath>
#include <iostream>
#include <chrono>

namespace duckdb {

BitmapFilter::BitmapFilter(ClientContext &context_p, const BitmapFilterConfig &config, bool parallel_build)
    : parallel_build(parallel_build) {
	context = &context_p;
	buffer_manager = &BufferManager::GetBufferManager(*context);
	config_ = config;
	const auto span = uint64_t(config.upper_bound) - uint64_t(config.lower_bound);
	const auto word_count = config.lower_bound <= config.upper_bound ? span / 64 + 1 : 1;

	buf_ = buffer_manager->GetBufferAllocator().Allocate(64 + word_count * sizeof(uint64_t));
	// make sure blocks is a 64-byte aligned pointer, i.e., cache-line aligned
	blocks = reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(buf_.get())) & ~63ULL);
	std::fill_n(blocks, word_count, 0);
}

 
template <typename T>
void BitmapFilter::BitmapFilterInsert(int num, const data_ptr_t BF_RESTRICT keys_ori, uint64_t *BF_RESTRICT bf) const {
	const auto keys = reinterpret_cast<const T *>(keys_ori);
	for (int i = 0; i < num; i++) {
		const auto key = keys[i];
		if (key < config_.lower_bound || key > config_.upper_bound) {
			continue;
		}
		const auto bit = uint64_t(key) - uint64_t(config_.lower_bound);
		const auto mask = uint64_t(1) << (bit % 64);
		auto &word = bf[bit / 64];
		if (parallel_build) {
			auto &atomic_word = *reinterpret_cast<std::atomic<uint64_t> *>(&word);
			// Bits only transition from zero to one during construction.
			if ((atomic_word.load(std::memory_order_relaxed) & mask) == 0) {
				atomic_word.fetch_or(mask, std::memory_order_relaxed);
			}
		} else {
			word |= mask;
		}
	}
}

template <typename T> 
int BitmapFilter::BitmapFilterLookup(int num, const data_ptr_t BF_RESTRICT keys_ori, const uint64_t *BF_RESTRICT bf,
	                       SelectionVector &results, size_t& result_count) const {
	const T *keys = reinterpret_cast<const T *>(keys_ori);
	result_count = 0;
	// std::cerr << config_.lower_bound << " " << config_.upper_bound << " " << keys[0] << " " << (blocks[0] & 1) << std::endl;
	for (int i = 0; i + SIMD_BATCH_SIZE <= num; i += SIMD_BATCH_SIZE) {
		bool outs[SIMD_BATCH_SIZE];
		for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
			bool flag = keys[i + j] >= config_.lower_bound && keys[i + j] <= config_.upper_bound;
			uint64_t bit_id = flag ? uint64_t(keys[i + j]) - uint64_t(config_.lower_bound) : 0;
			outs[j] = flag ? (blocks[bit_id >> 6] >> (bit_id & 63) & 1) : 0;
		}

		for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
			bool out = outs[j];
			results.set_index(result_count, i + j);
			result_count += out;
		}
	}

	// unaligned tail
	for (int i = num & ~(SIMD_BATCH_SIZE - 1); i < num; i++) {
		bool flag = keys[i] >= config_.lower_bound && keys[i] <= config_.upper_bound;
		uint64_t bit_id = flag ? uint64_t(keys[i]) - uint64_t(config_.lower_bound) : 0;
		bool out = flag ? (blocks[bit_id >> 6] >> (bit_id & 63) & 1) : 0;
		results.set_index(result_count, i);
		result_count += out;
	}
	return num;
}


template <typename T> 
int BitmapFilter::BitmapFilterLookup(int num, const data_ptr_t BF_RESTRICT keys_ori, const uint64_t *BF_RESTRICT bf,
	                       Vector &results, size_t& result_count) const {
	const T *keys = reinterpret_cast<const T *>(keys_ori);
	result_count = 0;
	// std::cerr << config_.lower_bound << " " << config_.upper_bound << " " << keys[0] << " " << (blocks[0] & 1) << std::endl;
	auto writer = FlatVector::Writer<bool>(results, num);
	for (int i = 0; i + SIMD_BATCH_SIZE <= num; i += SIMD_BATCH_SIZE) {
		bool outs[SIMD_BATCH_SIZE];
		for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
			bool flag = keys[i + j] >= config_.lower_bound && keys[i + j] <= config_.upper_bound;
			uint64_t bit_id = flag ? uint64_t(keys[i + j]) - uint64_t(config_.lower_bound) : 0;
			outs[j] = flag ? (blocks[bit_id >> 6] >> (bit_id & 63) & 1) : 0;
		}

		for (int j = 0; j < SIMD_BATCH_SIZE; j++) {
			bool out = outs[j];
			writer.WriteValue(out);
			result_count += out;
		}
	}

	// unaligned tail
	for (int i = num & ~(SIMD_BATCH_SIZE - 1); i < num; i++) {
		bool flag = keys[i] >= config_.lower_bound && keys[i] <= config_.upper_bound;
		uint64_t bit_id = flag ? uint64_t(keys[i]) - uint64_t(config_.lower_bound) : 0;
		bool out = flag ? (blocks[bit_id >> 6] >> (bit_id & 63) & 1) : 0;
		writer.WriteValue(out);
		result_count += out;
	}
	return num;
}

int BitmapFilter::Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, SelectionVector &results, size_t& result_count) const {
	int count = static_cast<int>(chunk.size());
	if (bound_cols_applied.size() != 1) {
		throw InternalException("RPT: Bitmap Filter needs bound_cols_applied.size() == 1!");
	}
	auto &v = chunk.data[bound_cols_applied[0]];
	v.Flatten(chunk.size());

	switch (v.GetType().InternalType()) {
	case PhysicalType::BOOL:
		BitmapFilterLookup<bool>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT8:
		BitmapFilterLookup<int8_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT16:
		BitmapFilterLookup<int16_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT32:
		BitmapFilterLookup<int32_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT64:
		BitmapFilterLookup<int64_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT8:
		BitmapFilterLookup<uint8_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT16:
		BitmapFilterLookup<uint16_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT32:
		BitmapFilterLookup<uint32_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT64:
		BitmapFilterLookup<uint64_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	default:
		throw InternalException("RPT: Cannot support this vector type in bitmap filter! column index=%d, type=%s", bound_cols_applied[0], v.GetType().ToString());
	}

	return count;
}


int BitmapFilter::Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, Vector &results, size_t& result_count) const {
	int count = static_cast<int>(chunk.size());
	if (bound_cols_applied.size() != 1) {
		throw InternalException("RPT: Bitmap Filter needs bound_cols_applied.size() == 1!");
	}
	auto &v = chunk.data[bound_cols_applied[0]];
	v.Flatten(chunk.size());

	switch (v.GetType().InternalType()) {
	case PhysicalType::BOOL:
		BitmapFilterLookup<bool>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT8:
		BitmapFilterLookup<int8_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT16:
		BitmapFilterLookup<int16_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT32:
		BitmapFilterLookup<int32_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::INT64:
		BitmapFilterLookup<int64_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT8:
		BitmapFilterLookup<uint8_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT16:
		BitmapFilterLookup<uint16_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT32:
		BitmapFilterLookup<uint32_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	case PhysicalType::UINT64:
		BitmapFilterLookup<uint64_t>(count, FlatVector::GetDataMutable(v), blocks, results, result_count);
		break;
	default:
		throw InternalException("RPT: Cannot support this vector type in bitmap filter! column index=%d, type=%s", bound_cols_applied[0], v.GetType().ToString());
	}

	return count;
}

void BitmapFilter::Insert(DataChunk &chunk, const vector<idx_t> &bound_cols_built) {
	int count = static_cast<int>(chunk.size());
	if (bound_cols_built.size() != 1) {
		throw InternalException("RPT: Bitmap Filter needs bound_cols_applied.size() == 1!");
	}

	auto &v = chunk.data[bound_cols_built[0]];
	v.Flatten(chunk.size());

	switch (v.GetType().InternalType()) {
	case PhysicalType::BOOL:
		BitmapFilterInsert<bool>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::INT8:
		BitmapFilterInsert<int8_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::INT16:
		BitmapFilterInsert<int16_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::INT32:
		BitmapFilterInsert<int32_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::INT64:
		BitmapFilterInsert<int64_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::UINT8:
		BitmapFilterInsert<uint8_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::UINT16:
		BitmapFilterInsert<uint16_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::UINT32:
		BitmapFilterInsert<uint32_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	case PhysicalType::UINT64:
		BitmapFilterInsert<uint64_t>(count, FlatVector::GetDataMutable(v), blocks);
		break;
	default:
		throw InternalException("RPT: Cannot support this vector type in bitmap filter!");
	}
}


template <typename T>
FilterPropagateResult BitmapFilter::BitmapFilterRangeLookup(const BaseStatistics& stats, const uint64_t *BF_RESTRICT bf) const {
	if (!NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	T min_value = NumericStats::GetMin<T>(stats);
	T max_value = NumericStats::GetMax<T>(stats);
	if (min_value > max_value) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE; // Invalid stats
	}	
	if (config_.lower_bound > config_.upper_bound || max_value < config_.lower_bound || min_value > config_.upper_bound) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	const auto lower = static_cast<uint64_t>(config_.lower_bound);
	const auto first = min_value < config_.lower_bound ? 0 : uint64_t(min_value) - lower;
	const auto last = (max_value > config_.upper_bound ? uint64_t(config_.upper_bound) : uint64_t(max_value)) - lower;
	const auto first_word = first / 64;
	const auto last_word = last / 64;
	const auto first_bits = bf[first_word] & (UINT64_MAX << (first % 64));
	const auto last_mask = UINT64_MAX >> (63 - last % 64);
	if (first_word == last_word) {
		return (first_bits & last_mask) ? FilterPropagateResult::NO_PRUNING_POSSIBLE
		                                : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (first_bits || (bf[last_word] & last_mask)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	for (auto i = first_word + 1; i < last_word; i++) {
		if (bf[i]) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
	}

	return FilterPropagateResult::FILTER_ALWAYS_FALSE;
}



FilterPropagateResult BitmapFilter::CheckStatistics(const BaseStatistics& stats) {
	switch (stats.GetType().InternalType()) {
	case PhysicalType::INT8:
		return BitmapFilterRangeLookup<int8_t>(stats, blocks);
	case PhysicalType::INT16:
		return BitmapFilterRangeLookup<int16_t>(stats, blocks);
	case PhysicalType::INT32:
		return BitmapFilterRangeLookup<int32_t>(stats, blocks);
	case PhysicalType::INT64:
		return BitmapFilterRangeLookup<int64_t>(stats, blocks);
	case PhysicalType::UINT8:
		return BitmapFilterRangeLookup<uint8_t>(stats, blocks);
	case PhysicalType::UINT16:
		return BitmapFilterRangeLookup<uint16_t>(stats, blocks);
	case PhysicalType::UINT32:
		return BitmapFilterRangeLookup<uint32_t>(stats, blocks);
	case PhysicalType::UINT64:
		return BitmapFilterRangeLookup<uint64_t>(stats, blocks);
	// case PhysicalType::INT128:
	// 	return BitmapFilterRangeLookup<hugeint_t>(stats, blocks);
	// case PhysicalType::UINT128:
	// 	return BitmapFilterRangeLookup<uhugeint_t>(stats, blocks);
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}


string BitmapFilter::ToString() const {
	stringstream ss;
	ss << "BitmapFilter=[";
	for (auto i = config_.lower_bound; i <= config_.upper_bound; i++) {
		auto id = i - config_.lower_bound;
		if (blocks[id >> 6] >> (id & 63) & 1) {
			ss << i << ", ";
		}
	}
	ss << "]";
	return ss.str();
}

} // namespace duckdb
