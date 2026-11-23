#include "predicate_transfer/transfer_bf_linker.hpp"
#include "predicate_transfer/table_operator_manager.hpp"

#include <algorithm>
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

#include "duckdb/planner/operator/logical_empty_result.hpp"
#include "predicate_transfer/operator/logical_use_bf.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

void TransferBFLinker::LinkBFOperators(LogicalOperator &op) {
	state = State::COLLECT_BF_CREATORS;
	VisitOperator(op);

	state = State::LINK_BF_USERS;
	VisitOperator(op);

	state = State::CLEAN_USELESS_OPERATORS;
	VisitOperator(op);

	state = State::UPDATE_MIN_MAX_BINDING;
	VisitOperator(op);
}

void TransferBFLinker::VisitOperator(LogicalOperator &op) {
	VisitOperatorInternal(op);
}
void TransferBFLinker::VisitOperatorInternal(LogicalOperator &op) {
	switch (state) {
	case State::COLLECT_BF_CREATORS: {
		if (TableOperatorManager::CheckLogicalCreateBF(op)) {
			auto &create_bf_op = op.Cast<LogicalCreateBF>();

			// Compact consecutive creators: creator --> creator --> scan ==> new_creator --> scan
			while (TableOperatorManager::CheckLogicalCreateBF(*create_bf_op.children[0])) {
				auto &child_creator = create_bf_op.children[0]->Cast<LogicalCreateBF>();
				for (auto &filter_plan : child_creator.filter_plans) {
					create_bf_op.AddFilterPlan(filter_plan);
				}
				create_bf_op.children[0] = std::move(child_creator.children[0]);
			}

			for (auto &filter_plan : create_bf_op.filter_plans) {
				bf_creators[filter_plan->plan_id] = &create_bf_op;
			}
			create_bf_op.min_max_to_create.resize(create_bf_op.filter_plans.size());
			create_bf_op.min_max_applied_cols.resize(create_bf_op.filter_plans.size());
			create_bf_op.scan_filter_columns.resize(create_bf_op.filter_plans.size());
		}
		break;
	}
	case State::LINK_BF_USERS: {
		if (TableOperatorManager::CheckLogicalUseBF(op)) {
			auto &bf_user = op.Cast<LogicalUseBF>();
			auto &filter_plan = bf_user.filter_plan;
			auto *related_creator = bf_creators[filter_plan->plan_id];

			if (related_creator != nullptr) {
				bf_user.related_create_bf = related_creator;
				useful_creator.insert(related_creator);
			}
		}
		break;
	}
	case State::CLEAN_USELESS_OPERATORS: {
		for (size_t i = 0; i < op.children.size(); i++) {
			auto &child = op.children[i];
			while (true) {
				if (TableOperatorManager::CheckLogicalCreateBF(*child) && !useful_creator.count(child.get())) {
					child = std::move(child->children[0]);
					continue;
				}
				if (TableOperatorManager::CheckLogicalUseBF(*child)) {
					auto &user = child->Cast<LogicalUseBF>();
					if (user.related_create_bf == nullptr) {
						auto moved_child = std::move(child); // Avoid use-after-move
						child = make_uniq<LogicalEmptyResult>(std::move(moved_child));
						break;
					}
				}
				break;
			}
		}
		break;
	}
	case State::UPDATE_MIN_MAX_BINDING: {
		if (TableOperatorManager::CheckLogicalUseBF(op)) {
			auto &bf_user = op.Cast<LogicalUseBF>();

			bool all_numerical = true;
			for (auto &type : bf_user.filter_plan->return_types) {
				if (!type.IsNumeric()) {
					all_numerical = false;
					break;
				}
			}

			bool pushed_into_scan = false;
			if (all_numerical) {
				vector<JoinFilterPushdownColumn> columns;
				for (auto &expr : bf_user.expressions) {
					JoinFilterPushdownColumn column;
					column.probe_column_index = expr->Cast<BoundColumnRefExpression>().Binding();
					columns.push_back(std::move(column));
				}

				shared_ptr<DynamicTableFilterSet> filter_set;
				UpdateMinMaxBinding(*bf_user.children[0], columns, filter_set);
				if (filter_set) {
					auto &key_types = bf_user.filter_plan->return_types;
					for (idx_t i = 0; i < columns.size(); i++) {
						auto &column = columns[i];
						if (RuntimeFilterCastUtil::GetRuntimeFilterInputType(column, key_types[i]) != key_types[i]) {
							filter_set.reset();
							break;
						}
					}
				}

				if (filter_set) {
					pushed_into_scan = columns.size() == 1;
					auto &creator = *bf_user.related_create_bf;
					const auto plan_idx = FindPlanIndex(bf_user.filter_plan, creator.filter_plans);
					creator.min_max_to_create[plan_idx] = std::move(filter_set);
					auto &bindings = creator.min_max_applied_cols[plan_idx];
					bindings.clear();
					for (const auto &column : columns) {
						bindings.push_back(column.probe_column_index);
					}
					creator.scan_filter_columns[plan_idx] = std::move(columns);
				}
			}
			bf_user.use_table_filter_bf &= pushed_into_scan;
		}
		break;
	}
	default:
		break;
	}

	for (auto &child : op.children) {
		VisitOperatorInternal(*child);
	}
}

idx_t TransferBFLinker::FindPlanIndex(const shared_ptr<FilterPlan> &plan,
                                      const vector<shared_ptr<FilterPlan>> &filter_plans) {
	idx_t plan_idx = std::numeric_limits<idx_t>::max();
	for (idx_t i = 0; i < filter_plans.size(); i++) {
		auto &creator_plan = filter_plans[i];
		if (creator_plan->plan_id == plan->plan_id) {
			plan_idx = i;
			break;
		}
	}
	return plan_idx;
}

void TransferBFLinker::UpdateMinMaxBinding(LogicalOperator &op, vector<JoinFilterPushdownColumn> &columns,
                                           shared_ptr<DynamicTableFilterSet> &filter_set) {
	auto &child = op;
	switch (child.type) {
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = child.Cast<LogicalGet>();
		if (!get.function.filter_pushdown) {
			return;
		}
		get.ResolveOperatorTypes();
		const auto scan_bindings = get.GetColumnBindings();
		for (auto &column : columns) {
			auto it = std::find(scan_bindings.begin(), scan_bindings.end(), column.probe_column_index);
			if (it == scan_bindings.end()) {
				return;
			}
			column.storage_type = get.types[it - scan_bindings.begin()];
		}

		if (!get.dynamic_filters) {
			get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
		}

		filter_set = get.dynamic_filters;
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &proj = child.Cast<LogicalProjection>();
		const auto output_bindings = proj.GetColumnBindings();
		for (auto &column : columns) {
			auto it = std::find(output_bindings.begin(), output_bindings.end(), column.probe_column_index);
			if (it == output_bindings.end()) {
				return;
			}
			const auto &expression = *proj.expressions[it - output_bindings.begin()];
			JoinFilterPushdownColumn input;
			if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(expression, input) ||
			    input.mode != JoinFilterPushdownMode::RECONSTRUCT_EXPRESSION) {
				return;
			}
			column.probe_column_index = input.probe_column_index;
			// The lower projection's casts must execute before the casts
			// already found at the consumer.
			column.runtime_filter_casts.insert(column.runtime_filter_casts.begin(),
			                                   input.runtime_filter_casts.begin(), input.runtime_filter_casts.end());
		}
		UpdateMinMaxBinding(*child.children[0], columns, filter_set);
		break;
	}
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		// does not affect probe side - recurse into left child
		UpdateMinMaxBinding(*child.children[0], columns, filter_set);
		break;
	}

	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
		if (TableOperatorManager::CheckLogicalCreateBF(child) || TableOperatorManager::CheckLogicalUseBF(child)) {
			UpdateMinMaxBinding(*child.children[0], columns, filter_set);
		}
		break;
	}
	default:
		break;
	}
}
} // namespace duckdb
