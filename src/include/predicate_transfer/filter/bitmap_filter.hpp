//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/predicate_transfer/filter/bitmap_filter.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/planner/column_binding.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cstdint>
#include <mutex>

#include "filter.hpp"

#ifndef BF_RESTRICT
#if defined(_MSC_VER)
#define BF_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define BF_RESTRICT __restrict__
#else
// Fallback: just return the pointer as-is
#define BF_RESTRICT
#endif
#endif

namespace duckdb {

class BitmapFilterConfig {
public:
	int64_t lower_bound = 0;
	int64_t upper_bound = 0;

	BitmapFilterConfig(int64_t lower_bound, int64_t upper_bound) : lower_bound(lower_bound), upper_bound(upper_bound) {
	}

	BitmapFilterConfig() = default;
};

class BitmapFilter : public RPTFilter {
public:
	BitmapFilter(ClientContext &context_p, const BitmapFilterConfig &config, bool parallel_build);

	ClientContext *context;
	BufferManager *buffer_manager;

	uint64_t *blocks;
	AllocatedData buf_;
	BitmapFilterConfig config_;
	const bool parallel_build;

public:
	static constexpr const int32_t SIMD_BATCH_SIZE = 16;

	int Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, SelectionVector &results, size_t& result_count) const override;

	int Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, Vector& results, size_t& result_count) const override;
	
	void Insert(DataChunk &chunk, const vector<idx_t> &bound_cols_built) override;
	
	FilterPropagateResult CheckStatistics(const BaseStatistics& stats) override;

	const BitmapFilterConfig& GetConfig() const { return config_; }

	template <typename T>
	int BitmapFilterLookup(int num, const data_ptr_t BF_RESTRICT keys_ori, const uint64_t *BF_RESTRICT bf,
	                       SelectionVector &results, size_t& result_count) const;

	template <typename T>
	int BitmapFilterLookup(int num, const data_ptr_t BF_RESTRICT keys_ori, const uint64_t *BF_RESTRICT bf,
	                       Vector& results, size_t& result_count) const;

	template <typename T>
	void BitmapFilterInsert(int num, const data_ptr_t BF_RESTRICT keys_ori, uint64_t *BF_RESTRICT bf) const;

	
	template <typename T>
	FilterPropagateResult BitmapFilterRangeLookup(const BaseStatistics& stats, const uint64_t *BF_RESTRICT bf) const;

	string ToString() const override;
};

} // namespace duckdb
