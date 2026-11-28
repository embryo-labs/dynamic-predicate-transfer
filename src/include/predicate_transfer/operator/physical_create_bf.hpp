//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/persistent/physical_create_bf.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "predicate_transfer/filter/filter.hpp"
#include "duckdb/common/radix_partitioning.hpp"
#include "predicate_transfer/dag.hpp"

namespace duckdb {
class CreateBFGlobalSinkState;

class PhysicalCreateBF : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTENSION;

public:
	class Config {
		public:
		 bool use_dynamic_in_filter;
		 bool use_table_filter_bf;
		 bool use_bitmap_filter;
	};
	PhysicalCreateBF(PhysicalPlan &physical_plan, vector<LogicalType> types,
	                 const vector<FilterPhysicalPlan> &filter_plans,
	                 vector<shared_ptr<DynamicTableFilterSet>> dynamic_filter_sets,
	                 vector<vector<ColumnBinding>> &dynamic_filter_cols,
	                 const vector<vector<JoinFilterPushdownColumn>> &scan_filter_columns, idx_t estimated_cardinality,
	                 bool is_probing_side, const Config& config);

	bool is_probing_side;
	shared_ptr<Pipeline> this_pipeline;
	vector<shared_ptr<Pipeline>> build_pipelines;
	void BuildFilterInput(Pipeline &current, MetaPipeline &meta_pipeline);
	vector<weak_ptr<Pipeline>> consumer_pipelines;
	bool CanAbandon(ClientContext &context, idx_t &source_rows) const;
	void RestoreConsumerPipelines() const;

	vector<FilterPhysicalPlan> filter_plans;
	Config config;

	//! Unified BFs contain all BFs, and they can be reused by PhysicalUseBF.
	struct VectorHash {
		size_t operator()(const std::vector<idx_t> &key) const {
			size_t hash = 0;
			for (const auto &elem : key) {
				hash ^= std::hash<idx_t> {}(elem) + (hash << 1); // Combine the hash values
			}
			return hash;
		}
	};
	unordered_map<vector<idx_t>, shared_ptr<RPTFilterWrapper>, VectorHash> unique_rpt_filters;
	vector<shared_ptr<RPTFilterUsage>> bf_to_create;

	//! Min-max Filters
	vector<vector<ColumnBinding>> min_max_applied_cols;
	vector<vector<JoinFilterPushdownColumn>> scan_filter_columns;
	vector<shared_ptr<DynamicTableFilterSet>> min_max_to_create;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                 GlobalSourceState &gstate) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

	bool ParallelSource() const override {
		return true;
	}

	ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const override;

public:
	// Sink interface
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;
	SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const override;
	SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                          OperatorSinkFinalizeInput &input) const override;

	bool HasEmptyBuild() const;

	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const override;

	bool IsSink() const override {
		return true;
	}

	bool ParallelSink() const override {
		return true;
	}

	// Add related createBF dependency
	void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) override;
	void BuildPipelinesFromRelated(Pipeline &current, MetaPipeline &meta_pipeline);

};
} // namespace duckdb
