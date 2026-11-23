#pragma once

#include "predicate_transfer/join_graph/table_operator_manager.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb::predicate_transfer_graph {

//===--------------------------------------------------------------------===//
// ColumnBindingHashFunc
//===--------------------------------------------------------------------===//
struct ColumnBindingHashFunc {
	size_t operator()(const ColumnBinding &key) const {
		return std::hash<uint64_t> {}(key.table_index.index) ^ (std::hash<uint64_t> {}(key.column_index) << 1U);
	}
};

//===--------------------------------------------------------------------===//
// EdgeInfo — describes one physical join between two base tables. The join
// may have multiple equality conditions; each condition contributes one entry
// to the parallel `*_bindings` / `return_types` vectors.
//===--------------------------------------------------------------------===//
class EdgeInfo {
public:
	vector<LogicalType> return_types;

	LogicalOperator &left_table;
	vector<ColumnBinding> left_bindings;

	LogicalOperator &right_table;
	vector<ColumnBinding> right_bindings;

	bool protect_left = false;
	bool protect_right = false;

	EdgeInfo(vector<LogicalType> types, LogicalOperator &left, vector<ColumnBinding> left_bindings,
	         LogicalOperator &right, vector<ColumnBinding> right_bindings)
	    : return_types(std::move(types)), left_table(left), left_bindings(std::move(left_bindings)), right_table(right),
	      right_bindings(std::move(right_bindings)) {
	}

public:
	idx_t KeyCount() const {
		return left_bindings.size();
	}

	bool operator==(const EdgeInfo &e) const {
		return return_types == e.return_types && left_bindings == e.left_bindings && right_bindings == e.right_bindings;
	}

	EdgeInfo Flip() const {
		EdgeInfo flipped(return_types, right_table, right_bindings, left_table, left_bindings);
		flipped.protect_left = protect_right;
		flipped.protect_right = protect_left;
		return flipped;
	}

	string ToString() const;
};

//===--------------------------------------------------------------------===//
// JoinKeyTableGroup — union-find group of tables sharing a join key
//===--------------------------------------------------------------------===//
struct JoinKeyTableGroup {
	unordered_map<idx_t, ColumnBinding> table_ids;
	//! Representative key type for this equivalence class. Set from the first
	//! join condition that seeded the group; used as the closure-edge type in
	//! DiscoverEdges, where we do not always have a reliable way to index into
	//! a table operator's output `types` (e.g. a compression projection may
	//! have shrunk `types` below the post-rename column_index).
	LogicalType return_type = LogicalType::INVALID;

	JoinKeyTableGroup(const idx_t table_id, ColumnBinding col, LogicalType type) : return_type(std::move(type)) {
		table_ids.emplace(table_id, col);
	}

	void Union(const JoinKeyTableGroup &other);
	string ToString() const;
};

//===--------------------------------------------------------------------===//
// Type aliases
//===--------------------------------------------------------------------===//
using BindingGroupMap = unordered_map<ColumnBinding, shared_ptr<JoinKeyTableGroup>, ColumnBindingHashFunc>;

//! Extract the join graph before either extension chooses a transfer strategy.
class JoinGraph {
public:
	explicit JoinGraph(ClientContext &context) : table_operator_manager(context) {
	}

	bool Build(LogicalOperator &plan, bool bundle_composite_edges);
	string EdgesToString() const;

	TableOperatorManager table_operator_manager;
	unordered_map<idx_t, unordered_map<idx_t, vector<EdgeInfo>>> neighbor_matrix;
	BindingGroupMap table_groups;

private:
	void ExtractEdgesInfo(const vector<reference<LogicalOperator>> &join_operators);
	void DiscoverEdges(bool bundle_composite_edges);
	void BundleCompositeEdges();
	void AddEdgePair(LogicalOperator &left_node, vector<ColumnBinding> left_bindings, LogicalOperator &right_node,
	                 vector<ColumnBinding> right_bindings, vector<LogicalType> forward_types, bool protect_left,
	                 bool protect_right);
};

} // namespace duckdb::predicate_transfer_graph
