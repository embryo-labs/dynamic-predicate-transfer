#include "predicate_transfer/push_bf_into_table.hpp"
#include "predicate_transfer/table_operator_manager.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"

#include <algorithm>

#include "predicate_transfer/operator/logical_create_bf.hpp"
#include "predicate_transfer/operator/logical_use_bf.hpp"

namespace duckdb {

RPTPushUseBFIntoTable::RPTPushUseBFIntoTable(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> RPTPushUseBFIntoTable::Optimize(unique_ptr<LogicalOperator> op) {
	TryOptimize(op);
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

// A probe on an inner theta join can run on the scan that supplies its keys.
// On an outer join, the preserved input supplies exactly the same set of build
// keys before and after the join, even when matches duplicate its rows.
static bool PushFilterIntoJoinScan(unique_ptr<LogicalOperator> &op, bool build) {
	auto &input = *op->children[0];
	const bool cross = input.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT;
	if (!cross && input.type != LogicalOperatorType::LOGICAL_ANY_JOIN) {
		return false;
	}
	idx_t preserved_side = DConstants::INVALID_INDEX;
	if (!cross) {
		auto &join = input.Cast<LogicalAnyJoin>();
		if (build) {
			if (join.join_type != JoinType::LEFT && join.join_type != JoinType::RIGHT) {
				return false;
			}
			preserved_side = join.join_type == JoinType::LEFT ? 0 : 1;
		} else if (join.join_type != JoinType::INNER || join.condition->IsVolatile() || join.condition->CanThrow()) {
			return false;
		}
	}
	for (idx_t i = 0; i < input.children.size(); i++) {
		auto &side = input.children[i];
		auto scan = side.get();
		if (cross && build && TableOperatorManager::CheckLogicalUseBF(*scan)) {
			scan = scan->children[0].get();
		}
		if (scan->type != LogicalOperatorType::LOGICAL_GET ||
		    (preserved_side != DConstants::INVALID_INDEX && i != preserved_side)) {
			continue;
		}
		const auto bindings = side->GetColumnBindings();
		auto supplies_keys = [&](const vector<ColumnBinding> &keys) {
			return !keys.empty() && std::all_of(keys.begin(), keys.end(), [&](const ColumnBinding &key) {
				return std::find(bindings.begin(), bindings.end(), key) != bindings.end();
			});
		};
		if (build) {
			const auto &plans = op->Cast<LogicalCreateBF>().filter_plans;
			if (plans.empty() || !std::all_of(plans.begin(), plans.end(), [&](const shared_ptr<FilterPlan> &plan) {
				    return !plan->exact_row_ids && supplies_keys(plan->build);
			    })) {
				continue;
			}
		} else if (!supplies_keys(op->Cast<LogicalUseBF>().filter_plan->apply)) {
			continue;
		}
		auto parent = std::move(op->children[0]);
		op->children[0] = std::move(side);
		side = std::move(op);
		op = std::move(parent);
		return true;
	}
	return false;
}

void RPTPushUseBFIntoTable::TryOptimize(unique_ptr<LogicalOperator> &op) {
	if (op->children.empty()) {
		return;
	}
	if (TableOperatorManager::CheckLogicalCreateBF(*op)) {
		op->Cast<LogicalCreateBF>().use_table_filter_bf = true;
		// Both operators may wrap the same product. Place the probe on its
		// scan first so the builder can consume the filtered keys there.
		auto &child = op->children[0];
		if (TableOperatorManager::CheckLogicalUseBF(*child) &&
		    child->children[0]->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			PushFilterIntoJoinScan(child, false);
		}
		PushFilterIntoJoinScan(op, true);
		return;
	}
	if (!TableOperatorManager::CheckLogicalUseBF(*op) || PushFilterIntoJoinScan(op, false)) {
		return;
	}
	auto *child = op->children[0].get();
	while (child->type == LogicalOperatorType::LOGICAL_FILTER || child->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		child = child->children[0].get();
	}
	// The linker validates aliases and reconstructs supported casts.
	op->Cast<LogicalUseBF>().use_table_filter_bf = child->type == LogicalOperatorType::LOGICAL_GET;
}

} // namespace duckdb
