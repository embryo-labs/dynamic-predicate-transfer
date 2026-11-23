#include "predicate_transfer/operator/logical_create_bf.hpp"
#include "predicate_transfer/operator/physical_create_bf.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include <utility>

namespace duckdb {

LogicalCreateBF::LogicalCreateBF(vector<shared_ptr<FilterPlan>> filter_plans)
    : LogicalExtensionOperator(), filter_plans(std::move(filter_plans)) {
	// create bound column ref expression for each build column
	// avoid to be removed by RemoveUnUsedColumnOptimizer
	for (auto &filter_plan : this->filter_plans) {
		for (idx_t i = 0; i < filter_plan->return_types.size(); i++) {
			auto col_expr = make_uniq<BoundColumnRefExpression>(filter_plan->return_types[i], filter_plan->build[i]);
			expressions.push_back(std::move(col_expr));
		}
	}
}

void LogicalCreateBF::AddFilterPlan(shared_ptr<FilterPlan> filter_plan) {
	for (idx_t i = 0; i < filter_plan->return_types.size(); i++) {
		auto col_expr = make_uniq<BoundColumnRefExpression>(filter_plan->return_types[i], filter_plan->build[i]);
		expressions.push_back(std::move(col_expr));
	}
	this->filter_plans.push_back(std::move(filter_plan));
}

InsertionOrderPreservingMap<string> LogicalCreateBF::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	result["Name"] = GetExtensionName();
	result["BF Number"] = std::to_string(filter_plans.size());
	string bfs;
	for (auto &bf_plan : filter_plans) {
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
	}
	result["BloomFilters"] = bfs;
	result["ID"] = std::to_string(reinterpret_cast<size_t>(this));

	string min_max_filter;
	for (auto &filter : min_max_applied_cols) {
		for (auto &v : filter) {
			min_max_filter += std::to_string(v.table_index.index) + "." + std::to_string(v.column_index) + " ";
		}
	}
	result["Min-Max Filter"] = min_max_filter;
	return result;
}

void LogicalCreateBF::Serialize(Serializer &serializer) const {
	LogicalExtensionOperator::Serialize(serializer);
	serializer.WritePropertyWithDefault<vector<shared_ptr<FilterPlan>>>(200, "filter_plans", filter_plans);
	serializer.WritePropertyWithDefault<vector<vector<ColumnBinding>>>(201, "min_max_applied_cols",
	                                                                   min_max_applied_cols);
	serializer.WritePropertyWithDefault<bool>(202, "use_dynamic_in_filter", use_dynamic_in_filter);
	serializer.WritePropertyWithDefault<bool>(203, "use_table_filter_bf", use_table_filter_bf);
	serializer.WritePropertyWithDefault<bool>(204, "use_bitmap_filter", use_bitmap_filter);
}

unique_ptr<LogicalExtensionOperator> LogicalCreateBF::Deserialize(Deserializer &deserializer) {
	vector<shared_ptr<FilterPlan>> filter_plans;
	deserializer.ReadPropertyWithDefault<vector<shared_ptr<FilterPlan>>>(200, "filter_plans", filter_plans);
	vector<vector<ColumnBinding>> min_max_applied_cols;
	deserializer.ReadPropertyWithDefault<vector<vector<ColumnBinding>>>(201, "min_max_applied_cols",
	                                                                    min_max_applied_cols);
	bool use_dynamic_in_filter;
	deserializer.ReadPropertyWithDefault<bool>(202, "use_dynamic_in_filter", use_dynamic_in_filter);
	bool use_table_filter_bf;
	deserializer.ReadPropertyWithDefault<bool>(203, "use_table_filter_bf", use_table_filter_bf);
	bool use_bitmap_filter;
	deserializer.ReadPropertyWithDefault<bool>(204, "use_bitmap_filter", use_bitmap_filter);
	auto ret = new LogicalCreateBF(std::move(filter_plans));
	ret->min_max_to_create.resize(min_max_applied_cols.size());
	ret->min_max_applied_cols = std::move(min_max_applied_cols);
	ret->use_bitmap_filter = use_bitmap_filter;
	ret->use_table_filter_bf = use_table_filter_bf;
	ret->use_dynamic_in_filter = use_dynamic_in_filter;
	return unique_ptr<LogicalExtensionOperator>(ret);
}

vector<ColumnBinding> LogicalCreateBF::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

void LogicalCreateBF::ResolveTypes() {
	types = children[0]->types;
}

void LogicalCreateBF::ResolveColumnBindings(ColumnBindingResolver &res, vector<ColumnBinding> &bindings) {
	for (auto &child : children) {
		res.VisitOperator(*child);
	}
	filter_physical_plans.clear();
	idx_t expr_id = 0;
	for (auto &bf_plan : filter_plans) {
		FilterPhysicalPlan physical_plan;
		physical_plan.logical_plan = bf_plan;
		physical_plan.bound_cols.clear();
		for (idx_t t = 0; t < bf_plan->build.size(); t++, expr_id++) {
			if (expr_id >= expressions.size()) {
				throw InternalException("Predicate Transfer: expr_id exceeds expressions.size()! At LogicalCreateBF %zu. expressions.size() = %d", reinterpret_cast<size_t>(this), expressions.size());
			}
			auto &col_expr = expressions[expr_id];
			auto &col_bind = col_expr->Cast<BoundColumnRefExpression>().Binding();
			bool found = false;
			for (idx_t i = 0; i < bindings.size(); i++) {
				if (col_bind == bindings[i]) {
					found = true;
					physical_plan.bound_cols.push_back(i);
					break;
				}
			}

			if (!found) {
				stringstream all_col_str;
				for (idx_t i = 0; i < bindings.size(); i++) {
					all_col_str << bindings[i].ToString() << ", ";
				}

				throw InternalException("Predicate Transfer: Failed to bind column reference %s. At LogicalCreateBF %zu. Column bindings are: [%s]\n", col_bind.ToString(), reinterpret_cast<uint64_t>(this), all_col_str.str());
			}
		}
		filter_physical_plans.push_back(physical_plan);
	}
	for (auto &expression : expressions) {
		res.VisitExpression(&expression);
	}
	bindings = GetColumnBindings();
}

void LogicalCreateBF::UpdateBindings() {
	idx_t expr_id = 0;
	for (auto &bf_plan : filter_plans) {
		for (auto &expr : bf_plan->build) {
			auto &v = expr;
			
			auto &col_expr = expressions[expr_id];
			auto &col_bind = col_expr->Cast<BoundColumnRefExpression>().Binding();
			v = col_bind;
			expr_id += 1;
		}
	}
}

PhysicalOperator &LogicalCreateBF::CreatePlan(ClientContext &context, PhysicalPlanGenerator &planner) {
	if (!physical) {
		auto &plan = planner.CreatePlan(*children[0]);
		auto &create_bf = planner.Make<PhysicalCreateBF>(plan.types, filter_physical_plans, min_max_to_create,
		                                                 min_max_applied_cols, scan_filter_columns, estimated_cardinality, can_stop, PhysicalCreateBF::Config{
																											.use_dynamic_in_filter=use_dynamic_in_filter, .use_table_filter_bf=use_table_filter_bf, .use_bitmap_filter=use_bitmap_filter});
		physical = static_cast<PhysicalCreateBF *>(&create_bf); // Ensure safe raw pointer storage
		create_bf.children.emplace_back(plan);
	}
	return *static_cast<PhysicalOperator *>(physical); // Ensure correct ownership
}

PhysicalCreateBF &LogicalCreateBF::CreatePlanFromRelated(ClientContext &context, PhysicalPlanGenerator &planner) {
	if (!physical) {
		auto &plan = planner.CreatePlan(*children[0]);
		auto &create_bf = planner.Make<PhysicalCreateBF>(plan.types, filter_physical_plans, min_max_to_create,
		                                                 min_max_applied_cols, scan_filter_columns, estimated_cardinality, can_stop, PhysicalCreateBF::Config{
																											.use_dynamic_in_filter=use_dynamic_in_filter, .use_table_filter_bf=use_table_filter_bf, .use_bitmap_filter=use_bitmap_filter});
		physical = static_cast<PhysicalCreateBF *>(&create_bf); // Ensure safe raw pointer storage
		create_bf.children.emplace_back(plan);
	}
	return *physical; // Ensure correct ownership
}

} // namespace duckdb