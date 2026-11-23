#pragma once

#include "duckdb/main/client_context.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb::predicate_transfer_graph {

class TableOperatorManager {
public:
	explicit TableOperatorManager(ClientContext &context) : context(context) {
	}

	ClientContext &context;

public:
	void Build(LogicalOperator &plan);
	const unordered_map<idx_t, reference<LogicalOperator>> &GetAllTableOperators() const {
		return table_operators;
	}
	const vector<reference<LogicalOperator>> &GetAllJoinOperators() const {
		return join_operators;
	}
	vector<reference<LogicalOperator>> GetTableInDFSOrder() const {
		return table_dfs_order;
	}

	LogicalOperator *GetTableOperator(idx_t table_idx);
	const LogicalOperator *GetTableOperator(idx_t table_idx) const;
	optional_ptr<LogicalOperator> GetCTEDefinition(LogicalOperator &scan, ColumnBinding key, ColumnBinding &output);
	bool CanScheduleTransfer(idx_t source, idx_t target) const;
	ColumnBinding GetRenaming(ColumnBinding col_binding);

	//! Clear stored operator references (call after plan modification to prevent dangling refs).
	void ClearTableOperators() {
		table_operators.clear();
		join_operators.clear();
		table_dfs_order.clear();
		contains_join_set_.clear();
		cte_origins.clear();
		cte_outputs.clear();
		cte_definitions.clear();
		table_scopes.clear();
		active_scopes.clear();
		join_keys.clear();
	}

	static bool ResolveScanKey(LogicalOperator &op, ColumnBinding &key, const LogicalType &type,
	                           TableOperatorManager *producers = nullptr);
	unordered_set<const LogicalOperator *> semi_senders;
	bool NeedsResultBinding(const LogicalOperator &op) const;
	static idx_t GetScalarTableIndex(const LogicalOperator &op);

	static string GetTableName(const LogicalOperator &op);
	string GetColumnName(const ColumnBinding &binding);

private:
	void AddTableOperator(LogicalOperator &op);

	//! Two-pass extraction:
	//!   Pass 1 (CollectJoins): walk the plan, collect inner equi-joins into
	//!     join_operators and build contains_join_set_ — the set of operators
	//!     whose subtree contains at least one inner equi-join.
	//!   Pass 2 (CollectTableOperators): admit any subtree NOT in
	//!     contains_join_set_ as a table operator. When descending through
	//!     contains_join_set_ operators, record PROJECTION/AGG renames so
	//!     upstream join conditions can resolve to the admitted subtree's
	//!     table_index.
	bool CollectJoins(LogicalOperator &op);
	void CollectTableOperators(LogicalOperator &op);
	void RecordRenameOneLevel(LogicalOperator &op);

	static bool IsInnerEquiJoin(const LogicalOperator &op);

	struct HashFunc {
		size_t operator()(const ColumnBinding &key) const {
			return std::hash<uint64_t> {}(key.table_index.index) ^ (std::hash<uint64_t> {}(key.column_index) << 1U);
		}
	};
	unordered_map<ColumnBinding, ColumnBinding, HashFunc> rename_col_bindings;
	unordered_set<ColumnBinding, HashFunc> join_keys;

	unordered_map<idx_t, reference<LogicalOperator>> cte_definitions;
	unordered_map<idx_t, vector<ColumnBinding>> cte_outputs;
	unordered_set<const LogicalOperator *> active_scopes;
	unordered_map<idx_t, unordered_set<const LogicalOperator *>> table_scopes;
	unordered_map<ColumnBinding, ColumnBinding, HashFunc> cte_origins;

	vector<reference<LogicalOperator>> join_operators;
	vector<reference<LogicalOperator>> table_dfs_order;
	unordered_map<idx_t, reference<LogicalOperator>> table_operators;

	//! Operators whose subtree contains at least one inner equi-join.
	//! Populated by CollectJoins (pass 1), consumed by CollectTableOperators
	//! (pass 2). Cleared on ClearTableOperators.
	unordered_set<const LogicalOperator *> contains_join_set_;
};
} // namespace duckdb::predicate_transfer_graph
