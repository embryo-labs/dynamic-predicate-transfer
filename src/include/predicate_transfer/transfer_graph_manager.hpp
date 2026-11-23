#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/common/vector.hpp"

#include "table_operator_manager.hpp"
#include "predicate_transfer/join_graph/join_graph.hpp"
#include "dag.hpp"
#include "config.hpp"

#include <iostream>

namespace duckdb {

class TransferGraphManager {
public:
	class EdgeInfo {
	public:
		EdgeInfo(const LogicalType &type, LogicalOperator &left, const ColumnBinding &left_binding,
		         LogicalOperator &right, const ColumnBinding &right_binding)
		    : return_type(type), left_table(left), left_binding(left_binding), right_table(right),
		      right_binding(right_binding) {
		}

		bool operator==(const EdgeInfo &e) const {
			return return_type == e.return_type && left_binding == e.left_binding && right_binding == e.right_binding;
		}

		LogicalType return_type;

		LogicalOperator &left_table;
		ColumnBinding left_binding;

		LogicalOperator &right_table;
		ColumnBinding right_binding;

		bool protect_left = false;
		bool protect_right = false;

		string ToString() const;
	};

	// Join Key Table Groups
	struct JoinKeyTableGroup {
		LogicalType return_type;
		unordered_map<idx_t, ColumnBinding> table_ids;
		idx_t leader_id;
		ColumnBinding leader_column_binding;

		JoinKeyTableGroup(const LogicalType &return_type, const idx_t table_id, ColumnBinding col)
		    : return_type(return_type), leader_id(std::numeric_limits<idx_t>::max()) {
			table_ids.emplace(table_id, col);
		}

		void Union(const JoinKeyTableGroup &other);

		void RegisterLeader(const idx_t table_id, const ColumnBinding &column_binding);

		string ToString() const;
	};

	struct ColumnBindingHashFunc {
		size_t operator()(const ColumnBinding &key) const {
			return std::hash<uint64_t> {}(key.table_index.index) ^ (std::hash<uint64_t> {}(key.column_index) << 1);
		}
	};

	using JoinKeySet = unordered_set<ColumnBinding, ColumnBindingHashFunc>;
	using TableJoinKeyMap = unordered_map<idx_t, JoinKeySet>;
	using BindingGroupMap = unordered_map<ColumnBinding, shared_ptr<JoinKeyTableGroup>, ColumnBindingHashFunc>;

	explicit TransferGraphManager(ClientContext &context, RPTOptimizerConfig config) : context(context), join_graph(context), table_operator_manager(join_graph.table_operator_manager), config(config) {
	}

	ClientContext &context;
	predicate_transfer_graph::JoinGraph join_graph;
	predicate_transfer_graph::TableOperatorManager &table_operator_manager;
	TransferGraph transfer_graph;
	vector<LogicalOperator *> transfer_order;
	RPTOptimizerConfig config;

	struct TransferStep {
		LogicalOperator* table;
		vector<shared_ptr<GraphEdge>> create_bf;
	};

	vector<TransferStep> result_transfer_steps;

public:
	bool Build(LogicalOperator &op);
	void AddFilterPlan(idx_t create_table, const shared_ptr<FilterPlan> &filter_plan, bool reverse);
	string TransferPlanToString() const;
	string EdgesToString() const;
	string TablesToString() const;

private:
	void PrepareTransferKeys();

private:
	void CreateTransferPlan();
	void CreateTransferSteps();
	vector<TransferStep> ReorderTransferSteps(vector<TransferStep> steps) const;
	vector<TransferStep> CreateExtraTransfer();
	vector<TransferStep> CreateTransferStepBasedOnNodeOrder(const vector<LogicalOperator*>& nodes);
	// Strategy on generating edges and order of nodes of transfer graph
	struct TransferStrategy {
		vector<idx_t> transfer_order_id;
		vector<pair<idx_t, idx_t>> transfer_edge_id;
	};
	TransferStrategy HeuristicLargestRoot(const vector<reference<LogicalOperator>> &sorted_nodes) const;

private:
	//! Classify all tables into three categories: intermediate table, unfiltered table, and filtered table.
	void ClassifyTables();
	bool CheckIsTableFilterSimple(const TableFilter& table_filter);
	bool CheckAllSimpleTableFilters(const LogicalOperator& table);
	bool CheckTableAllNumericProjection(const LogicalOperator& table);
	void PruneUnfilteredTransfers();
	bool CanTransfer(ColumnBinding from, ColumnBinding to) const;
	void AllowTransfer(ColumnBinding from, ColumnBinding to, const LogicalType &type);

	//! From table id to its join keys
	TableJoinKeyMap table_join_keys;
	//! From join keys to its table groups
	BindingGroupMap table_groups;
	//! Table categories
	unordered_set<idx_t> unfiltered_table;
	unordered_set<idx_t> filtered_table;
	unordered_set<idx_t> intermediate_table;

private:
	unordered_map<idx_t, unordered_map<idx_t, vector<EdgeInfo>>> neighbor_matrix;
};

} // namespace duckdb
