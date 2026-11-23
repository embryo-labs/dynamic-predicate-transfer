#include <utility>

#include "predicate_transfer/operator/logical_dynamic_select.hpp"
#include "predicate_transfer/operator/physical_dynamic_select.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

LogicalDynamicSelect::LogicalDynamicSelect() : LogicalExtensionOperator() {
}

InsertionOrderPreservingMap<string> LogicalDynamicSelect::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	result["Name"] = GetExtensionName();
	return result;
}

void LogicalDynamicSelect::Serialize(Serializer &serializer) const {
	LogicalExtensionOperator::Serialize(serializer);
}

unique_ptr<LogicalOperator> LogicalDynamicSelect::Deserialize(Deserializer &deserializer) {
	return unique_ptr<LogicalOperator>(new LogicalDynamicSelect());
}

vector<ColumnBinding> LogicalDynamicSelect::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

void LogicalDynamicSelect::ResolveTypes() {
	types = children[0]->types;
}

void LogicalDynamicSelect::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	res.VisitOperator(*children[1]);
	res.VisitOperator(*children[0]);
	for (auto &expression : expressions) {
		res.VisitExpression(&expression);
	}
	bindings = GetColumnBindings();
}

PhysicalOperator &LogicalDynamicSelect::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	auto &lhs = planner.CreatePlan(*children[0]);
	auto &rhs = planner.CreatePlan(*children[1]);
	auto &op = planner.Make<PhysicalDynamicSelect>(lhs.types, estimated_cardinality);
	op.children.emplace_back(lhs);
	op.children.emplace_back(rhs);
	return op;
}

} // namespace duckdb
