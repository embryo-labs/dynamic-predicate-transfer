#include "predicate_transfer/reorder_mark_join.hpp"
#include "predicate_transfer/table_operator_manager.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

RPTReorderMarkJoin::RPTReorderMarkJoin(Optimizer &optimizer) : optimizer(optimizer) {
}

unique_ptr<LogicalOperator> RPTReorderMarkJoin::Optimize(unique_ptr<LogicalOperator> op) {
	TryOptimize(op);
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

void RPTReorderMarkJoin::TryOptimize(unique_ptr<LogicalOperator> &op) {
	if (op->children.empty()) {
		return;
	}

	// Reorder BF-related operators above a MARK join:
	// From: op -> filter -> join(MARK) -> create_bf_chain -> real_child
	// To:   op -> create_bf_chain -> filter -> join(MARK) -> real_child
	for (auto &child : op->children) {
		if (child->type != LogicalOperatorType::LOGICAL_FILTER) {
			continue;
		}
		auto &filter = child->Cast<LogicalFilter>();
		if (filter.expressions.size() != 1 ||
		    filter.expressions[0]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF || filter.children.empty() ||
		    filter.children[0]->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			continue;
		}

		auto &join = filter.children[0]->Cast<LogicalComparisonJoin>();
		if (join.join_type != JoinType::MARK || !(TableOperatorManager::CheckLogicalCreateBF(*join.children[0]) ||
		                                          TableOperatorManager::CheckLogicalUseBF(*join.children[0]))) {
			continue;
		}

		// Step 1: Identify the last CREATE_BF in the BF chain (which may include USE_BF in between)
		LogicalOperator *bf_tail = join.children[0].get();
		LogicalOperator *last_creator = nullptr;

		while (TableOperatorManager::CheckLogicalCreateBF(*bf_tail) ||
		       TableOperatorManager::CheckLogicalUseBF(*bf_tail)) {
			if (TableOperatorManager::CheckLogicalCreateBF(*bf_tail)) {
				last_creator = bf_tail;
			}
			bf_tail = bf_tail->children[0].get();
		}

		// If there is no CREATE_BF in the chain, skip the transformation
		if (!last_creator) {
			continue;
		}

		bf_tail = last_creator;
		D_ASSERT(TableOperatorManager::CheckLogicalCreateBF(*bf_tail));

		// Step 2: Detach the BF chain from the join and isolate the real child that follows it
		auto bf_chain = std::move(join.children[0]);
		auto real_child = std::move(bf_tail->children[0]);

		// Step 3: Rewire the filter subtree under the end of the BF chain
		bf_tail->children[0] = std::move(child);

		// Step 4: Fix the join's left child to point to the real child
		auto &new_filter = bf_tail->children[0]->Cast<LogicalFilter>();
		auto &new_join = new_filter.children[0]->Cast<LogicalComparisonJoin>();
		new_join.children[0] = std::move(real_child);

		// Step 5: Replace op's child with the head of the new reordered BF chain
		child = std::move(bf_chain);
	}

	return;
}

} // namespace duckdb
