//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/physical_use_bf.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/persistent/physical_create_bf.hpp"

namespace duckdb {
class PhysicalUseBF : public CachingPhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::USE_BF;
	static constexpr const idx_t COMPACTION_THRESHOLD = 256;

public:
	PhysicalUseBF(vector<LogicalType> types, const shared_ptr<FilterPlan> &filter_plan, unique_ptr<BloomFilterUsage> bf,
	              PhysicalCreateBF *related_create_bfs, idx_t estimated_cardinality, bool below_join);

	shared_ptr<FilterPlan> filter_plan;
	PhysicalCreateBF *related_creator = nullptr;

	shared_ptr<BloomFilterUsage> bf_to_use;

public:
	// Operator interface
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;

	bool ParallelOperator() const override {
		return true;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;

protected:
	OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                                   GlobalOperatorState &gstate, OperatorState &state) const override;
};
} // namespace duckdb
