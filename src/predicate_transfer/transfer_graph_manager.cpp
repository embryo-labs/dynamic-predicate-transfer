#include "duckdb/planner/filter/expression_filter.hpp"
#include "predicate_transfer/transfer_graph_manager.hpp"

#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "predicate_transfer/predicate_transfer_optimizer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

#include <queue>
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"

namespace duckdb {

void TransferGraphManager::JoinKeyTableGroup::Union(const TransferGraphManager::JoinKeyTableGroup &other) {
	D_ASSERT(return_type == other.return_type);
	table_ids.insert(other.table_ids.begin(), other.table_ids.end());
}

void TransferGraphManager::JoinKeyTableGroup::RegisterLeader(const idx_t table_id,
                                                             const ColumnBinding &column_binding) {
	if (std::numeric_limits<idx_t>::max() == leader_id) {
		leader_id = table_id;
		leader_column_binding = column_binding;
	}
}

string TransferGraphManager::JoinKeyTableGroup::ToString() const {
	stringstream ss;
	ss << "(leader=" << leader_id << ", ids=[";
	for (auto &id : table_ids) {
		ss << "(" << id.first << ", " << id.second.ToString() << "), ";
	}
	ss << "])";
	return ss.str();
}

bool TransferGraphManager::Build(LogicalOperator &plan) {
	if (!join_graph.Build(plan, true)) {
		return false;
	}

	PrepareTransferKeys();
	ClassifyTables();
	if (config.skip_unfiltered_table_flag) {
		PruneUnfilteredTransfers();
	}
	CreateTransferPlan();
	CreateTransferSteps();
	return true;
}

string TransferGraphManager::TransferPlanToString() const {
	// Output table groups
	stringstream ss;
	unordered_set<JoinKeyTableGroup *> visited;
	for (auto &pair : table_groups) {
		auto &group = pair.second;
		if (visited.count(group.get())) {
			continue;
		}
		visited.insert(group.get());
		ss << group->ToString();
	}

	ss << "digraph G {" << '\n';

	// Create a map to store outgoing neighbors for each LogicalOperator
	std::unordered_map<std::string, std::vector<std::pair<std::string, std::pair<int, int>>>> outgoing_neighbors;

	// Output nodes (LogicalOperators) and their outgoing neighbors (edges)
	for (const auto &op : transfer_order) {
		std::string op_name = TableOperatorManager::GetTableName(
		    *op); // Use TableOperatorManager::GetTableName function for operator name
		ss << "\t(\"" << op_name << "\", id=" << TableOperatorManager::GetScalarTableIndex(*op) << ");\n";

		// Output edges to each neighbor (only outgoing edges)
		if (outgoing_neighbors.find(op_name) != outgoing_neighbors.end()) {
			for (const auto &neighbor : outgoing_neighbors[op_name]) {
				std::string neighbor_name = neighbor.first;
				int left_column_id = neighbor.second.first;
				int right_column_id = neighbor.second.second;

				// Print edge with column ids
				ss << "\t\t\"" << op_name << "\" -> \"" << neighbor_name << "\" [label=\"Column " << left_column_id
				   << " -> Column " << right_column_id << "\"];\n";
			}
		}
	}

	ss << "}"
	   << "\n";

	ss << "TransferGraphForward={\n";
	// Output nodes (LogicalOperators) and their outgoing neighbors (edges)
	for (const auto &op : transfer_order) {
		std::string op_name = TableOperatorManager::GetTableName(
		    *op); // Use TableOperatorManager::GetTableName function for operator name
		ss << "\t\"" << op_name << ", id=" << TableOperatorManager::GetScalarTableIndex(*op) << "\";\n";
		idx_t id = TableOperatorManager::GetScalarTableIndex(*op);

		for (const auto &neighbor : (transfer_graph.find(id)->second)->forward_stage_edges.out) {
			std::string neighbor_name =
			    TableOperatorManager::GetTableName(*table_operator_manager.GetTableOperator(neighbor->destination));
			for (idx_t i = 0; i < neighbor->left.size(); i++) {
				idx_t left_column_id = neighbor->left[i].column_index;
				idx_t right_column_id = neighbor->right[i].column_index;
				if (neighbor->right[i].table_index.index == id) {
					std::swap(left_column_id, right_column_id);
				} else if (neighbor->left[i].table_index.index == id) {
				} else {
					throw InternalException("Invalid graph!");
				}

				// Print edge with column ids
				ss << "\t\t\"" << op_name << "\" -> (\"" << neighbor_name << "\", id=" << neighbor->destination
				   << ") [label=\"Column " << left_column_id << " -> Column " << right_column_id << "\"];\n";
			}
		}
	}
	ss << "}"
	   << "\n";

	ss << "TransferGraphBackward={\n";
	// Output nodes (LogicalOperators) and their outgoing neighbors (edges)
	for (const auto &op : transfer_order) {
		std::string op_name = TableOperatorManager::GetTableName(
		    *op); // Use TableOperatorManager::GetTableName function for operator name
		ss << "\t\"" << op_name << ", id=" << TableOperatorManager::GetScalarTableIndex(*op) << "\";\n";
		idx_t id = TableOperatorManager::GetScalarTableIndex(*op);

		for (const auto &neighbor : (transfer_graph.find(id)->second)->backward_stage_edges.out) {
			std::string neighbor_name =
			    TableOperatorManager::GetTableName(*table_operator_manager.GetTableOperator(neighbor->destination));
			for (idx_t i = 0; i < neighbor->left.size(); i++) {
				idx_t left_column_id = neighbor->left[i].column_index;
				idx_t right_column_id = neighbor->right[i].column_index;
				if (neighbor->right[i].table_index.index == id) {
					std::swap(left_column_id, right_column_id);
				} else if (neighbor->left[i].table_index.index == id) {
				} else {
					throw InternalException("Invalid graph!");
				}

				// Print edge with column ids
				ss << "\t\t\"" << op_name << "\" -> (\"" << neighbor_name << "\", id=" << neighbor->destination
				   << ") [label=\"Column " << left_column_id << " -> Column " << right_column_id << "\"];\n";
			}
		}
	}
	ss << "}"
	   << "\n";
	return ss.str();
}

void TransferGraphManager::PrepareTransferKeys() {
	// The RPT strategy assigns a leader per equality class and schedules
	// keys individually. Its private view does not change the shared graph.
	unordered_map<const predicate_transfer_graph::JoinKeyTableGroup *, shared_ptr<JoinKeyTableGroup>> groups;
	for (const auto &entry : join_graph.table_groups) {
		const auto &source = *entry.second;
		auto &group = groups[&source];
		if (!group) {
			group = make_shared_ptr<JoinKeyTableGroup>(source.return_type, entry.first.table_index.index, entry.first);
			group->table_ids = source.table_ids;
		}
		table_groups.emplace(entry.first, group);
	}
	for (const auto &source : join_graph.neighbor_matrix) {
		for (const auto &target : source.second) {
			auto &keys = neighbor_matrix[source.first][target.first];
			for (const auto &edge : target.second) {
				for (idx_t i = 0; i < edge.KeyCount(); i++) {
					keys.emplace_back(edge.return_types[i], edge.left_table, edge.left_bindings[i], edge.right_table,
					                  edge.right_bindings[i]);
					keys.back().protect_left = edge.protect_left;
					keys.back().protect_right = edge.protect_right;
				}
			}
		}
	}
	// A CTE consumer cannot build a filter before its producer is scheduled.
	// The producer's scan keys form a superset and can send to an independent
	// input. Only the original join direction may use this shortcut.
	for (const auto &from : join_graph.neighbor_matrix) {
		for (const auto &to : from.second) {
			if (table_operator_manager.CanScheduleTransfer(from.first, to.first)) {
				continue;
			}
			for (const auto &edge : to.second) {
				for (idx_t i = 0; !edge.protect_right && i < edge.KeyCount(); i++) {
					auto source = edge.left_bindings[i];
					auto target = edge.right_bindings[i];
					if (predicate_transfer_graph::TableOperatorManager::ResolveScanKey(edge.left_table, source, edge.return_types[i], &table_operator_manager) &&
					    predicate_transfer_graph::TableOperatorManager::ResolveScanKey(edge.right_table, target, edge.return_types[i]) &&
					    source.table_index != target.table_index &&
					    table_operator_manager.GetTableOperator(source.table_index.index) &&
					    table_operator_manager.GetTableOperator(target.table_index.index) &&
					    table_operator_manager.CanScheduleTransfer(source.table_index.index, target.table_index.index)) {
						AllowTransfer(source, target, edge.return_types[i]);
					}
				}
			}
		}
	}
}

static bool CheckIfCompareExpression(const Expression &expr) {
	return expr.GetExpressionType() == ExpressionType::COMPARE_EQUAL ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_NOTEQUAL ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_LESSTHAN ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_GREATERTHAN ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_LESSTHANOREQUALTO ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_GREATERTHANOREQUALTO ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_DISTINCT_FROM ||
	       expr.GetExpressionType() == ExpressionType::COMPARE_BETWEEN;
}

bool TransferGraphManager::CheckIsTableFilterSimple(const TableFilter &table_filter) {
	if (table_filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return false;
	}
	const auto &expr = *table_filter.Cast<ExpressionFilter>().expr;
	bool simple = true;
	std::function<void(const Expression &)> inspect = [&](const Expression &child) {
		auto type = child.GetExpressionType();
		if (!CheckIfCompareExpression(child) && type != ExpressionType::BOUND_REF &&
		    type != ExpressionType::VALUE_CONSTANT && type != ExpressionType::CONJUNCTION_AND &&
		    type != ExpressionType::CONJUNCTION_OR && type != ExpressionType::OPERATOR_IS_NULL &&
		    type != ExpressionType::OPERATOR_IS_NOT_NULL) {
			simple = false;
		}
		ExpressionIterator::EnumerateChildren(child, inspect);
	};
	inspect(expr);
	return simple;
}
bool TransferGraphManager::CheckAllSimpleTableFilters(const LogicalOperator& table) {
	if (table.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = table.Cast<LogicalGet>();
		// has filters
		if (!!get.table_filters.HasFilters()) {
			bool is_simple_filter = true;
			for (auto &filter_pair : get.table_filters) {
				auto &filter = filter_pair.Filter();
				if (!CheckIsTableFilterSimple(filter)) {
					is_simple_filter = false;
					break;
				}
			}
			return is_simple_filter;
		}
		// does not have efficient filters
		return false;
	} else if (table.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = table.Cast<LogicalFilter>();
		bool is_simple_filter = true;
		for (auto &expression : filter.expressions) {
			if (!CheckIfCompareExpression(*expression)) {
				is_simple_filter = false;
			}
			ExpressionIterator::EnumerateChildren(*expression, [&](Expression &child) {
				if (child.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					// pass
				} else if (child.GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
					// pass
				} else {
					is_simple_filter = false;
				}
			});
		}
		if (!is_simple_filter) {
			return false;
		}
		return CheckAllSimpleTableFilters(*table.children[0]);
	}
	return false;
}

void TransferGraphManager::ClassifyTables() {
	for (auto &entry : table_operator_manager.GetAllTableOperators()) {
		const auto id = entry.first;
		auto &table = entry.second.get();
		auto &keys = table_join_keys[id];
		unordered_set<const JoinKeyTableGroup *> groups;
		idx_t ungrouped = 0;
		for (auto &pair : neighbor_matrix[id]) {
			for (auto &edge : pair.second) {
				if (!keys.insert(edge.left_binding).second) {
					continue;
				}
				auto group = table_groups.find(edge.left_binding);
				if (group == table_groups.end()) {
					ungrouped++;
				} else {
					groups.insert(group->second.get());
				}
			}
		}
		// Predicates on the join key restrict rows just like predicates on
		// payload columns. Derived relations may already contain filters.
		if (table.type != LogicalOperatorType::LOGICAL_GET || table.Cast<LogicalGet>().table_filters.HasFilters()) {
			filtered_table.insert(id);
		} else if (groups.size() + ungrouped > 1) {
			intermediate_table.insert(id);
		} else {
			unfiltered_table.insert(id);
		}
	}
}

bool TransferGraphManager::CanTransfer(ColumnBinding from, ColumnBinding to) const {
	auto source = neighbor_matrix.find(from.table_index.index);
	if (source == neighbor_matrix.end() || !table_operator_manager.CanScheduleTransfer(from.table_index.index, to.table_index.index)) {
		return false;
	}
	auto dest = source->second.find(to.table_index.index);
	return dest != source->second.end() && std::any_of(dest->second.begin(), dest->second.end(), [&](const EdgeInfo &edge) {
		return edge.left_binding == from && edge.right_binding == to && !edge.protect_right;
	});
}

void TransferGraphManager::AllowTransfer(ColumnBinding from, ColumnBinding to, const LogicalType &type) {
	auto update = [&](ColumnBinding left, ColumnBinding right, bool forward) {
		auto &edges = neighbor_matrix[left.table_index.index][right.table_index.index];
		for (auto &edge : edges) {
			if (edge.left_binding == left && edge.right_binding == right) {
				(forward ? edge.protect_right : edge.protect_left) = false;
				return;
			}
		}
		auto &a = *table_operator_manager.GetTableOperator(left.table_index.index);
		auto &b = *table_operator_manager.GetTableOperator(right.table_index.index);
		edges.emplace_back(type, a, left, b, right);
		edges.back().protect_left = forward;
		edges.back().protect_right = !forward;
	};
	update(from, to, true);
	update(to, from, false);
}

void TransferGraphManager::PruneUnfilteredTransfers() {
	// Work on RPT+'s private graph. A shortcut must follow allowed
	// directions and the same column across every unfiltered relation.
	unordered_map<ColumnBinding, JoinKeySet, ColumnBindingHashFunc> outgoing;
	unordered_map<ColumnBinding, LogicalType, ColumnBindingHashFunc> key_types;
	for (const auto &source : neighbor_matrix) {
		for (const auto &dest : source.second) {
			for (const auto &edge : dest.second) {
				if (!edge.protect_right && table_operator_manager.CanScheduleTransfer(source.first, dest.first)) {
					outgoing[edge.left_binding].insert(edge.right_binding);
					key_types.emplace(edge.left_binding, edge.return_type);
				}
			}
		}
	}
	for (const auto &source : outgoing) {
		if (unfiltered_table.count(source.first.table_index.index)) {
			continue;
		}
		JoinKeySet visited = source.second;
		vector<ColumnBinding> pending(visited.begin(), visited.end());
		while (!pending.empty()) {
			auto key = pending.back();
			pending.pop_back();
			auto next = outgoing.find(key);
			if (!unfiltered_table.count(key.table_index.index) || next == outgoing.end()) {
				continue;
			}
			for (auto dest : next->second) {
				if (dest.table_index == source.first.table_index || !visited.insert(dest).second) {
					continue;
				}
				AllowTransfer(source.first, dest, key_types.at(source.first));
				pending.push_back(dest);
			}
		}
	}

	// A multi-key intermediate can relay a filter received on another key.
	// Without a path from a filtered source it has nothing to transmit.
	auto active = filtered_table;
	vector<idx_t> pending(active.begin(), active.end());
	while (!pending.empty()) {
		auto from = pending.back();
		pending.pop_back();
		for (const auto &dest : neighbor_matrix[from]) {
			if (intermediate_table.count(dest.first) && !active.count(dest.first) &&
			    table_operator_manager.CanScheduleTransfer(from, dest.first) &&
			    std::any_of(dest.second.begin(), dest.second.end(), [](const EdgeInfo &e) { return !e.protect_right; })) {
				active.insert(dest.first);
				pending.push_back(dest.first);
			}
		}
	}
	for (auto &source : neighbor_matrix) {
		for (auto it = source.second.begin(); it != source.second.end();) {
			auto &edges = it->second;
			vector<EdgeInfo> allowed;
			for (auto &edge : edges) {
				edge.protect_left |= !active.count(it->first);
				edge.protect_right |= !active.count(source.first);
				if (!edge.protect_left || !edge.protect_right) {
					allowed.push_back(edge);
				}
			}
			edges.swap(allowed);
			if (edges.empty()) {
				it = source.second.erase(it);
			} else {
				++it;
			}
		}
	}
}

TransferGraphManager::TransferStrategy
TransferGraphManager::HeuristicLargestRoot(const vector<reference<LogicalOperator>> &sorted_nodes) const {
	unordered_set<idx_t> constructed_set, unconstructed_set;
	set<pair<pair<idx_t, idx_t>, pair<idx_t, idx_t>>> candidate_edges;
	TransferStrategy result;
	auto tables = table_operator_manager.GetAllTableOperators();
	// Check unfiltered table first
	bool unfiltered_prior_flag = false;
	// Init the unconstructed_set
	for (auto &entry : tables) {
		idx_t id = entry.first;
		unconstructed_set.insert(id);
	}
	while (!tables.empty()) {
		idx_t root = std::numeric_limits<idx_t>::max();
		// Try to find the root
		{
			if (unfiltered_prior_flag) {
				for (idx_t i = 0; i < sorted_nodes.size(); i++) {
					auto id = table_operator_manager.GetScalarTableIndex(sorted_nodes[i]);
					if ((unfiltered_table.count(id)) && !constructed_set.count(id)) {
						root = id;
						break;
					}
				}
				if (root == std::numeric_limits<idx_t>::max()) {
					// there is no unfiltered table
					// reset root_ptr
					// find the root again
					unfiltered_prior_flag = false;
				}
			}

			if (!unfiltered_prior_flag) {
				for (idx_t i = 0; i < sorted_nodes.size(); i++) {
					auto id = table_operator_manager.GetScalarTableIndex(sorted_nodes[i]);
					if (!constructed_set.count(id)) {
						root = id;
						break;
					}
				}
			}
		}

		// If cannot find root then there must be something wrong
		if (root == std::numeric_limits<idx_t>::max()) {
			throw InternalException("RPT: Failed to find root in transfer graph");
		}

		// Add root
		idx_t step = 0;
		result.transfer_order_id.push_back(root);
		constructed_set.insert(root);
		tables.erase(root);
		{
			auto root_it = neighbor_matrix.find(root);
			if (root_it != neighbor_matrix.end()) {
				for (const auto &node : root_it->second) {
					idx_t id = node.first;
					if (!constructed_set.count(id)) {
						idx_t cardinality = transfer_graph.find(id)->second->cardinality_order;
						candidate_edges.insert(make_pair(make_pair(cardinality, step), make_pair(root, id)));
					}
				}
			}
		}

		// Register leader for backward pass
		{
			auto root_it = table_join_keys.find(root);
			if (root_it != table_join_keys.end()) {
				for (auto &col_binding : root_it->second) {
					auto it = table_groups.find(col_binding);
					if (it == table_groups.end()) {
						continue;
					}
					auto &group = it->second;
					if (group) {
						group->RegisterLeader(root, col_binding);
					}
				}
			}
		}

		// Build graph
		while (!candidate_edges.empty() && !unconstructed_set.empty()) {
			// Find a table in the unconstructed_set
			// Find edges with maximum weight.
			while (!candidate_edges.empty() && constructed_set.count(candidate_edges.rbegin()->second.second)) {
				candidate_edges.erase(std::prev(candidate_edges.end()));
			}
			// Check if there is no edge to the unconstructed_set
			if (candidate_edges.empty()) {
				break;
			}

			// The selected table in the unconstructed set
			auto selected_edge = candidate_edges.rbegin()->second;

			// move the node into constructed_set
			// erase the edge, add edges from this node
			candidate_edges.erase(std::prev(candidate_edges.end()));
			auto node_id = selected_edge.second;
			tables.erase(node_id);
			unconstructed_set.erase(node_id);
			constructed_set.insert(node_id);
			step += 1;

			{
				auto it = neighbor_matrix.find(node_id);
				if (it != neighbor_matrix.end()) {
					for (const auto &node : it->second) {
						idx_t id = node.first;
						if (!constructed_set.count(id)) {
							idx_t cardinality = transfer_graph.find(id)->second->cardinality_order;
							candidate_edges.insert(make_pair(make_pair(cardinality, step), make_pair(node_id, id)));
						}
					}
				}
			}

			result.transfer_order_id.push_back(node_id);
			result.transfer_edge_id.push_back(selected_edge);
			// set leader
			{
				auto node_it = table_join_keys.find(node_id);
				if (node_it != table_join_keys.end()) {
					for (auto &col_binding : node_it->second) {
						auto col_it = table_groups.find(col_binding);
						if (col_it != table_groups.end()) {
							auto &group = col_it->second;
							if (group) {
								group->RegisterLeader(node_id, col_binding);
							}
						}
					}
				}
			}
		}
	}

	return result;
}


vector<TransferGraphManager::TransferStep> TransferGraphManager::ReorderTransferSteps(vector<TransferGraphManager::TransferStep> steps) const {
	vector<TransferStep> result;
	result.reserve(steps.size());
	unordered_map<idx_t, vector<pair<LogicalOperator*, shared_ptr<GraphEdge>>>> create_bfs;
	for (auto& step : steps) {
		auto& table = step.table;
		auto id = TableOperatorManager::GetScalarTableIndex(*table);
		for (auto& edge_pair : create_bfs[id]) {
			auto src_table = edge_pair.first;
			auto edge = edge_pair.second;
			if (!result.empty() && result.back().table == src_table) {
				result.back().create_bf.push_back(edge);
			} else {
				result.push_back(TransferStep{.table=src_table, .create_bf=vector<shared_ptr<GraphEdge>>{edge}});
			}
		}
		create_bfs[id].clear();
		for (auto& edge : step.create_bf) {
			create_bfs[edge->destination].push_back(make_pair(table, edge));
		}
	}

	for (auto& data_pair : create_bfs) {
		for (auto& edge_pair : data_pair.second) {
			auto src_table = edge_pair.first;
			auto edge = edge_pair.second;
			if (!result.empty() && result.back().table == src_table) {
				result.back().create_bf.push_back(edge);
			} else {
				result.push_back(TransferStep{.table=src_table, .create_bf=vector<shared_ptr<GraphEdge>>{edge}});
			}
		}
	}
	return result;
}

void TransferGraphManager::CreateTransferSteps() {
	result_transfer_steps.clear();
	auto extra = vector<TransferStep>();
	if (config.use_extra_transfer) {
		extra = CreateExtraTransfer();
	}
	auto yannakakis = CreateTransferStepBasedOnNodeOrder(transfer_order);

	result_transfer_steps = extra;
	result_transfer_steps.insert(result_transfer_steps.end(), yannakakis.begin(), yannakakis.end());
	result_transfer_steps = ReorderTransferSteps(result_transfer_steps);
	if (config.skip_unfiltered_table_flag) {
		// The forest can omit a candidate incoming edge or schedule it later.
		// Only relay from a node reached in the actual transfer sequence.
		auto reached = filtered_table;
		for (auto &step : result_transfer_steps) {
			if (!reached.count(TableOperatorManager::GetScalarTableIndex(*step.table))) {
				step.create_bf.clear();
				continue;
			}
			for (const auto &edge : step.create_bf) {
				reached.insert(edge->destination);
			}
		}
	}
}

bool TransferGraphManager::CheckTableAllNumericProjection(const LogicalOperator& table) {
	if (table.type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = table.Cast<LogicalGet>();
		bool result = true;
		for (idx_t i = 0; i < get.returned_types.size() && result; i++) {
			if (!get.returned_types[i].IsNumeric()) {
				result = false;
			}
		}
		return result;
	} else {
		bool result = true;
		for (idx_t i = 0; i < table.children.size() && result; i++) {
			result &= CheckTableAllNumericProjection(*table.children[i]);
		}
		return result;
	}
}

vector<TransferGraphManager::TransferStep> TransferGraphManager::CreateExtraTransfer() {

	auto table_operators = table_operator_manager.GetTableInDFSOrder();
	std::sort(table_operators.begin(), table_operators.end(), [](reference<LogicalOperator> x, reference<LogicalOperator> y) {
		return x.get().estimated_cardinality < y.get().estimated_cardinality;
	});
	
	double total_cost = 0;
	double total_cardinality = 0;
	for (auto& table : table_operators) {
		total_cardinality += table.get().estimated_cardinality;
	}

	double cost_threshold = 0.1 * total_cardinality;
	unordered_set<idx_t> all_small_tables;
	unordered_set<idx_t> visited;
	double current_total_cardinality = 0;
	for (auto& table : table_operators) {
		auto table_id = TableOperatorManager::GetScalarTableIndex(table);
		if (!filtered_table.count(table_id)) {
			continue;
		}
		// if (!CheckTableAllNumericProjection(table)) {
		// 	continue;
		// }
		double cost = CheckAllSimpleTableFilters(table) && CheckTableAllNumericProjection(table) ? 1 : 10;
		if (current_total_cardinality + table.get().estimated_cardinality * cost < cost_threshold) {
			current_total_cardinality += table.get().estimated_cardinality;
			all_small_tables.insert(table_id);
		}
	}

	// std::cerr << current_total_cardinality << " " << total_cardinality << std::endl;

	vector<idx_t> all_nodes;

	for (auto& table : table_operators) {
		auto table_id = TableOperatorManager::GetScalarTableIndex(table);
		if (filtered_table.count(table_id) && all_small_tables.count(table_id)) {
			vector<idx_t> path;
			if (!visited.count(table_id)) {
				std::queue<idx_t> Q;
				Q.push(table_id);
				visited.insert(table_id);
				while (!Q.empty()) {
					auto front = Q.front();
					path.push_back(front);
					Q.pop();
					auto& edges = transfer_graph[front]->forward_stage_edges.out;
					for (auto& edge : edges) {
						auto dest = edge->destination;
						if (!visited.count(dest) && all_small_tables.count(dest)) {
							Q.push(dest);
							visited.insert(dest);
						}
					}
					
					auto& edges2 = transfer_graph[front]->backward_stage_edges.out;
					for (auto& edge : edges2) {
						auto dest = edge->destination;
						if (!visited.count(dest) && all_small_tables.count(dest)) {
							Q.push(dest);
							visited.insert(dest);
						}
					}
				}
			}

			// if (path.size() > 1) 
			{
				all_nodes.insert(all_nodes.end(), path.begin(), path.end());
			}
		}
	}

	// create transfer step
	vector<TransferStep> result;
	for (auto& id : all_nodes) {
		auto table = table_operator_manager.GetTableOperator(id);
		auto create_bfs = transfer_graph[id]->forward_stage_edges.out;
		auto step = TransferStep{.table = table};
		for (auto& create_bf : create_bfs) {
			// if (all_small_tables.count(create_bf->destination)) {
				step.create_bf.push_back(create_bf);
			// }
		}
		result.push_back(std::move(step));
	}

	for (auto it = all_nodes.rbegin(); it != all_nodes.rend(); it++) {
		auto id = *it;
		auto table = table_operator_manager.GetTableOperator(id);
		auto create_bfs = transfer_graph[id]->backward_stage_edges.out;
		auto step = TransferStep{.table = table};
		for (auto& create_bf : create_bfs) {
			// if (all_small_tables.count(create_bf->destination)) {
				step.create_bf.push_back(create_bf);
			// }
		}
		result.push_back(std::move(step));
	}

	return result;
}

vector<TransferGraphManager::TransferStep> TransferGraphManager::CreateTransferStepBasedOnNodeOrder(const vector<LogicalOperator*>& nodes) {
	vector<TransferStep> result;
	for (auto it = nodes.rbegin(); it != nodes.rend(); it++) {
		auto table = *it;
		auto id = TableOperatorManager::GetScalarTableIndex(*table);
		auto create_bf = transfer_graph[id]->forward_stage_edges.out;
		result.push_back(TransferStep{.table = table, .create_bf = create_bf});
	}

	for (auto& table : nodes) {
		auto id = TableOperatorManager::GetScalarTableIndex(*table);
		auto create_bf = transfer_graph[id]->backward_stage_edges.out;
		result.push_back(TransferStep{.table = table, .create_bf = create_bf});
	}

	return result;
}

void TransferGraphManager::CreateTransferPlan() {
	// auto sorted_table_operators = vector<reference<LogicalOperator>>();
	// for (auto &data : table_operator_manager.GetAllTableOperators()) {
	// 	auto &op = data.second;
	// 	sorted_table_operators.push_back(op);
	// }

	

	// Empty Result Data structure
	transfer_graph.clear();
	transfer_order.clear();

	// The sequence lists nodes in a left-to-right order in the tree (=join order)
	vector<reference<LogicalOperator>> sorted_table_operators;
	for (auto &entry : table_operator_manager.GetTableInDFSOrder()) {
		auto id = TableOperatorManager::GetScalarTableIndex(entry.get());
		if (table_operator_manager.GetTableOperator(id) == &entry.get()) {
			sorted_table_operators.push_back(entry);
		}
	}
	if (config.use_largest_root_order_flag) {
		// sort table by the estimated cardinality
		sort(sorted_table_operators.begin(), sorted_table_operators.end(),
	     [&](const LogicalOperator &a, const LogicalOperator &b) {
		     return a.estimated_cardinality > b.estimated_cardinality;
	     });
	} else if (config.use_estimated_cost_order_flag) {
		sort(sorted_table_operators.begin(), sorted_table_operators.end(),
	     [&](const LogicalOperator &a, const LogicalOperator &b) {
					double costa = CheckAllSimpleTableFilters(a) ? 1 : 3;
					double costb = CheckAllSimpleTableFilters(b) ? 1 : 3;
		     return a.estimated_cardinality * costa > b.estimated_cardinality * costb;
	     });
	}

	int priority = (int)sorted_table_operators.size();
	for (auto &op : sorted_table_operators) {
		auto id = TableOperatorManager::GetScalarTableIndex(op);
		// std::cerr << op.get().estimated_cardinality << " " << id << " " << TableOperatorManager::GetTableName(op) << std::endl;
		transfer_graph.emplace(id, unique_ptr<GraphNode>(new GraphNode(id, priority)));
		priority -= 1;
	}

	auto strategy = HeuristicLargestRoot(sorted_table_operators);

	for (auto &id : strategy.transfer_order_id) {
		transfer_order.push_back(table_operator_manager.GetTableOperator(id));
	}

	for (auto &edge_id_pair : strategy.transfer_edge_id) {
		auto edges = neighbor_matrix[edge_id_pair.first][edge_id_pair.second];
		if (edges.empty()) {
			throw InternalException("RPT: Transfer edge is nullptr");
		}

		idx_t left_idx = edge_id_pair.first;
		idx_t right_idx = edge_id_pair.second;

		D_ASSERT(left_idx != std::numeric_limits<idx_t>::max() && right_idx != std::numeric_limits<idx_t>::max());

		for (auto &edge : edges) {
			auto &type = edge.return_type;
			auto left_node = transfer_graph[left_idx].get();
			auto right_node = transfer_graph[right_idx].get();

			auto left_col = edge.left_binding;
			auto right_col = edge.right_binding;

			auto protect_left = edge.protect_left;
			auto protect_right = edge.protect_right;

			// forward: from the smaller to the larger
			if (!protect_left && table_operator_manager.CanScheduleTransfer(right_idx, left_idx)) {
				left_node->Add(right_node->id, left_col, right_col, type, true, true);
				right_node->Add(left_node->id, left_col, right_col, type, true, false);
			}

			// backward: from the larger to the smaller
			if (!protect_right && table_operator_manager.CanScheduleTransfer(left_idx, right_idx)) {
				auto group = table_groups.count(left_col) ? table_groups[left_col] : nullptr;
				if (group && group->leader_id != right_idx && CanTransfer(group->leader_column_binding, right_col)) {
					auto group_leader = group->leader_id;
					auto &leader_col = group->leader_column_binding;
					auto leader = transfer_graph[group_leader].get();

					right_node->Add(group_leader, right_col, leader_col, type, false, true);
					leader->Add(right_node->id, right_col, leader_col, type, false, false);
				} else {
					left_node->Add(right_node->id, left_col, right_col, type, false, false);
					right_node->Add(left_node->id, left_col, right_col, type, false, true);
				}
			}
		}
	}
}

string TransferGraphManager::EdgeInfo::ToString() const {
	stringstream ss;
	ss << "(return_type=";
	ss << return_type.ToString();
	ss << ", left_table=";
	ss << TableOperatorManager::GetScalarTableIndex(left_table);
	ss << ", left_binding=";
	ss << left_binding.ToString();
	ss << ", right_table=";
	ss << TableOperatorManager::GetScalarTableIndex(right_table);
	ss << ", right_binding=";
	ss << right_binding.ToString();
	ss << ", protect_left=" << protect_left;
	ss << ", protect_right=" << protect_right;
	ss << ")";
	return ss.str();
}

string TransferGraphManager::EdgesToString() const {
	return join_graph.EdgesToString();
}

string TransferGraphManager::TablesToString() const {
	stringstream ss;

	ss << "all_tables=(";
	for (auto &op : table_operator_manager.GetTableInDFSOrder()) {
		auto id = TableOperatorManager::GetScalarTableIndex(op);
		ss << "(id=" << id << ", name=" << TableOperatorManager::GetTableName(op);
		if (unfiltered_table.count(id)) {
			ss << " (No filtered)";
		}
		ss << "), ";
	}
	ss << ")\n";
	ss << "transfer_order=(";
	for (auto &op : transfer_order) {
		ss << "(id=" << TableOperatorManager::GetScalarTableIndex(*op)
		   << ", name=" << TableOperatorManager::GetTableName(*op) << "), ";
	}
	ss << ")";
	return ss.str();
}

} // namespace duckdb
