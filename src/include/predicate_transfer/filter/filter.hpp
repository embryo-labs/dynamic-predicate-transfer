//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/predicate_transfer/filter/filter.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/planner/column_binding.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"


namespace duckdb {

class RPTFilter {
public:
	RPTFilter() : finalized_(false) {
	}

	bool IsValid() const {
		return finalized_;
	}

	void SetValid() {
		finalized_ = true;
	}

	virtual int Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, SelectionVector &results, size_t& result_count) const = 0;
	
	virtual int Lookup(DataChunk &chunk, const vector<idx_t> &bound_cols_applied, Vector& results, size_t& result_count) const = 0;

	virtual void Insert(DataChunk &chunk, const vector<idx_t> &bound_cols_built) = 0;

	virtual FilterPropagateResult CheckStatistics(const BaseStatistics& stats) { return FilterPropagateResult::NO_PRUNING_POSSIBLE; }

	virtual ~RPTFilter() = default;

	virtual string ToString() const { return "RPTFilter"; };

	bool finalized_ = false;
};

class RPTFilterWrapper {
public:
	shared_ptr<RPTFilter> filter;
};

class RPTFilterUsage {
public:
	RPTFilterUsage(shared_ptr<RPTFilterWrapper> filter)
	    : filter(std::move(filter)) {
	}

	bool IsValid() const {
		return filter->filter && filter->filter->IsValid();
	}

	void SetValid() {
		filter->filter->SetValid();
	}

	shared_ptr<RPTFilter> GetFilter() {
		return filter->filter;
	}

public:
	int Lookup(DataChunk &chunk, const vector<idx_t>& bound_cols_applied, SelectionVector &results, size_t& result_count) const {
		return filter->filter->Lookup(chunk, bound_cols_applied, results, result_count);
	}
	void Insert(DataChunk &chunk, const vector<idx_t>& bound_cols_applied) const {
		return filter->filter->Insert(chunk, bound_cols_applied);
	}

private:
	shared_ptr<RPTFilterWrapper> filter;
};

} // namespace duckdb
