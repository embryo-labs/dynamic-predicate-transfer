#pragma once

#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "predicate_transfer/dag.hpp"

namespace duckdb {
class LogicalCreateBF;

class LogicalDynamicSelect : public LogicalExtensionOperator {

public:
	constexpr static const char *kExtensionName = "LogicalDynamicSelect";

	explicit LogicalDynamicSelect();

	PhysicalOperator &CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) override;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);
	void ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) override;

	vector<ColumnBinding> GetColumnBindings() override;

	string GetExtensionName() const override {
		return kExtensionName;
	}

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
