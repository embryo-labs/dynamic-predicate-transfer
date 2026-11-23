#include <utility>

#include "predicate_transfer/operator/physical_use_bf.hpp"
#include "predicate_transfer/operator/logical_use_bf.hpp"
#include "predicate_transfer/operator/logical_create_bf.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

LogicalUseBF::LogicalUseBF(shared_ptr<FilterPlan> filter_plan)
    : LogicalExtensionOperator(), filter_plan({std::move(filter_plan)}) {
	// create bound column ref expression for each apply column
	// avoid to be removed by RemoveUnUsedColumnOptimizer
	for (idx_t i = 0; i < this->filter_plan->return_types.size(); i++) {
		auto col_expr =
		    make_uniq<BoundColumnRefExpression>(this->filter_plan->return_types[i], this->filter_plan->apply[i]);
		expressions.push_back(std::move(col_expr));
	}
}

InsertionOrderPreservingMap<string> LogicalUseBF::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	result["Name"] = GetExtensionName();
	result["BF Number"] = std::to_string(1);
	string bfs;
	auto &bf_plan = filter_plan;
	bfs += "0x" + std::to_string(reinterpret_cast<uint64_t>(bf_plan.get())) + "\n";
	bfs += "Build: ";
	for (auto &expr : bf_plan->build) {
		auto &v = expr;
		bfs += std::to_string(v.table_index.index) + "." + std::to_string(v.column_index) + " ";
	}
	bfs += " Apply: ";
	for (auto &expr : bf_plan->apply) {
		auto &v = expr;
		bfs += std::to_string(v.table_index.index) + "." + std::to_string(v.column_index) + " ";
	}
	bfs += "\n";

	result["BloomFilters"] = bfs;
	result["BF Creator"] = std::to_string(reinterpret_cast<size_t>(related_create_bf));
	return result;
}

void LogicalUseBF::Serialize(Serializer &serializer) const {
	LogicalExtensionOperator::Serialize(serializer);
	serializer.WritePropertyWithDefault<shared_ptr<FilterPlan>>(200, "filter_plan", filter_plan);
	serializer.WritePropertyWithDefault<bool>(203, "use_table_filter_bf", use_table_filter_bf);
}

unique_ptr<LogicalExtensionOperator> LogicalUseBF::Deserialize(Deserializer &deserializer) {
	shared_ptr<FilterPlan> filter_plan;
	deserializer.ReadPropertyWithDefault<shared_ptr<FilterPlan>>(200, "filter_plan", filter_plan);
	bool use_table_filter_bf;
	deserializer.ReadPropertyWithDefault<bool>(203, "use_table_filter_bf", use_table_filter_bf);
	auto ret = new LogicalUseBF(filter_plan);
	ret->use_table_filter_bf = use_table_filter_bf;
	return unique_ptr<LogicalExtensionOperator>(ret);
}

vector<ColumnBinding> LogicalUseBF::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

void LogicalUseBF::ResolveTypes() {
	types = children[0]->types;
}


void LogicalUseBF::UpdateBindings() {
	idx_t expr_id = 0;
	for (auto &expr : filter_plan->apply) {
		auto &v = expr;
		
		auto &col_expr = expressions[expr_id];
		auto &col_bind = col_expr->Cast<BoundColumnRefExpression>().Binding();
		v = col_bind;
		expr_id += 1;
	}
}

void LogicalUseBF::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	for (auto &child : children) {
		res.VisitOperator(*child);
	}
	auto &bf_plan = filter_plan;
	filter_physical_plan.logical_plan = bf_plan;
	filter_physical_plan.bound_cols.clear();
	for (auto &col_expr : expressions) {
		auto &col_bind = col_expr->Cast<BoundColumnRefExpression>().Binding();
		bool found = false;
		for (idx_t i = 0; i < bindings.size(); i++) {
			if (col_bind == bindings[i]) {
				found = true;
				filter_physical_plan.bound_cols.push_back(i);
				break;
			}
		}
		if (!found) {
			throw InternalException("Predicate Transfer: Failed to bind column reference %s.\n", col_bind.ToString());
		}
	}
	for (auto &expression : expressions) {
		res.VisitExpression(&expression);
	}
	bindings = GetColumnBindings();
}

PhysicalOperator &LogicalUseBF::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	auto &plan = planner.CreatePlan(*children[0]); // Generate child plan
	auto &bf_creator = related_create_bf->CreatePlanFromRelated(context, planner);

	auto &bf_plan = filter_plan;
	size_t target_idx = std::numeric_limits<size_t>::max();
	for (size_t i = 0; i < bf_creator.filter_plans.size(); i++) {
		auto &filter_plan = bf_creator.filter_plans[i];
		if (filter_plan.logical_plan->plan_id == bf_plan->plan_id) {
			target_idx = i;
			break; // Found the target, exit loop
		}
	}
	
	if (target_idx == std::numeric_limits<size_t>::max()) {
		throw InternalException("Predicate Transfer: Failed to bind bf.\n");
	}

	auto &use_bf = planner.Make<PhysicalUseBF>(plan.types, filter_physical_plan, bf_creator.bf_to_create[target_idx],
	                                           &bf_creator, estimated_cardinality, use_table_filter_bf);
	use_bf.children.emplace_back(plan);
	return use_bf;
}

} // namespace duckdb