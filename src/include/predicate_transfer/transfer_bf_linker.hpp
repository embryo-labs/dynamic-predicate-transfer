//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/transfer_bf_linker.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "operator/logical_create_bf.hpp"

namespace duckdb {

//! This class is to link each PhysicalUseBF with its PhysicalCreateBF. It uses the FilterPlan information instead of
//! shared ptr to link.
class TransferBFLinker {
public:
	TransferBFLinker() : state(State::COLLECT_BF_CREATORS) {
	}

	void LinkBFOperators(LogicalOperator &op);

protected:
	void VisitOperator(LogicalOperator &op);

	static idx_t FindPlanIndex(const shared_ptr<FilterPlan> &plan, const vector<shared_ptr<FilterPlan>> &filter_plans);

	static void UpdateMinMaxBinding(LogicalOperator &op, vector<JoinFilterPushdownColumn> &columns,
	                                shared_ptr<DynamicTableFilterSet> &filter_set);

	void VisitOperator(LogicalOperator &op, bool is_probing_side);

protected:
	void VisitOperatorInternal(LogicalOperator &op);
	enum class State { COLLECT_BF_CREATORS, LINK_BF_USERS, CLEAN_USELESS_OPERATORS, UPDATE_MIN_MAX_BINDING };
	State state;

	unordered_set<LogicalOperator *> useful_creator;
	// filter_plan_id -> LogicalCreateBF
	unordered_map<idx_t, LogicalCreateBF *> bf_creators;
};
} // namespace duckdb
