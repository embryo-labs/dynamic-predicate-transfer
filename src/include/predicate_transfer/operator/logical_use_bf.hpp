#pragma once

#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "predicate_transfer/dag.hpp"

namespace duckdb {
class LogicalCreateBF;

class LogicalUseBF : public LogicalExtensionOperator {

public:
	constexpr static const char *kExtensionName = "LogicalRPTUseBF";

	explicit LogicalUseBF(shared_ptr<FilterPlan> filter_plans);

	shared_ptr<FilterPlan> filter_plan;
	FilterPhysicalPlan filter_physical_plan;
	LogicalCreateBF *related_create_bf = nullptr;
	bool use_table_filter_bf = false;

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer);
	void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;
	void UpdateBindings(); // update bindings from expressions because table ID may be changed by Late Materialization
	
	vector<ColumnBinding> GetColumnBindings() override;

	string GetExtensionName() const override {
		return kExtensionName;
	}

	class OperatorExtension : public duckdb::OperatorExtension {
	public:
		std::string GetName() override {
			return kExtensionName;
		}
		unique_ptr<LogicalExtensionOperator> Deserialize(Deserializer &deserializer) override {
			return LogicalUseBF::Deserialize(deserializer);
		}

	};

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
