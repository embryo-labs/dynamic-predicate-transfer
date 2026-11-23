//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_create_bf.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "predicate_transfer/dag.hpp"

namespace duckdb {
class PhysicalCreateBF;

class LogicalCreateBF : public LogicalExtensionOperator {
public:
	constexpr static const char *kExtensionName = "LogicalRPTCreateBF";

	explicit LogicalCreateBF(vector<shared_ptr<FilterPlan>> filter_plans);

	bool can_stop = false;
	vector<shared_ptr<FilterPlan>> filter_plans;
	vector<FilterPhysicalPlan> filter_physical_plans;
	PhysicalCreateBF *physical = nullptr;
	bool use_dynamic_in_filter = true;
	bool use_table_filter_bf = true;
	bool use_bitmap_filter = true;

	vector<shared_ptr<DynamicTableFilterSet>> min_max_to_create;
	vector<vector<ColumnBinding>> min_max_applied_cols;
	//! Runtime-only. Like the scan filter sets, these are re-linked for the plan.
	vector<vector<JoinFilterPushdownColumn>> scan_filter_columns;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;
	PhysicalCreateBF &CreatePlanFromRelated(ClientContext &context, PhysicalPlanGenerator &planner);
	void AddFilterPlan(shared_ptr<FilterPlan> filter_plan);

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;
	vector<ColumnBinding> GetColumnBindings() override;
	void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;
	void UpdateBindings(); // update bindings from expressions because table ID may be changed by Late Materialization
	
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer);

	string GetExtensionName() const override {
		return kExtensionName;
	}

	
	class OperatorExtension : public duckdb::OperatorExtension {
	public:
		std::string GetName() override {
			return kExtensionName;
		}
		unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override {
			return LogicalCreateBF::Deserialize(deserializer);
		}

	};

protected:
	void ResolveTypes() override;
};

} // namespace duckdb
