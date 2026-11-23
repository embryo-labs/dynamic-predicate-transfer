//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/physical_use_bf.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "physical_create_bf.hpp"

namespace duckdb {
class PhysicalUseBF : public CachingPhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

public:
	PhysicalUseBF(PhysicalPlan &physical_plan, vector<LogicalType> types, const FilterPhysicalPlan &filter_plan,
	              shared_ptr<RPTFilterUsage> filter, PhysicalCreateBF *related_create_bfs, idx_t estimated_cardinality, bool use_table_filter_bf);

	FilterPhysicalPlan filter_plan;
	PhysicalCreateBF *related_creator = nullptr;

	shared_ptr<RPTFilterUsage> filter_to_use;
	bool use_table_filter_bf = false;

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
