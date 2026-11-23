#include "predicate_transfer/predicate_transfer_optimizer.hpp"
#include "predicate_transfer/transfer_bf_linker.hpp"
#include "predicate_transfer/operator/logical_use_bf.hpp"
#include "predicate_transfer/late_materialization.hpp"
#include "predicate_transfer/reorder_mark_join.hpp"
#include "predicate_transfer/push_bf_into_table.hpp"

#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"

namespace duckdb {

// A window exposes input and computed bindings. HAVING also preserves both
// grouping and aggregate bindings. Give the completed result a single table
// index so joins on any of its outputs can participate in transfer.
static void BindIntermediateOutputs(unique_ptr<LogicalOperator> &op, LogicalOperator &root, Optimizer &optimizer,
                                    predicate_transfer_graph::TableOperatorManager &tables,
                                    bool has_join_above = false, bool can_receive = false) {
	if (op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return;
	}
	const bool join = op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
	                  op->type == LogicalOperatorType::LOGICAL_DELIM_JOIN;
	const bool below_join = has_join_above || join;
	for (idx_t side = 0; side < op->children.size(); side++) {
		auto &child = op->children[side];
		// A branch preserved by every enclosing join can only send outward.
		// Build from its surviving semi-join rows. Keep a receiving branch
		// at scan level so its incoming filters can still prune the scan.
		bool child_can_receive = can_receive;
		if (join) {
			const auto type = op->Cast<LogicalComparisonJoin>().join_type;
			const auto preserved = side == 0 ? JoinType::LEFT : JoinType::RIGHT;
			child_can_receive |= type != preserved;
		}
		if (below_join && !child_can_receive &&
		    child->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
			const auto type = child->Cast<LogicalComparisonJoin>().join_type;
			if (type == JoinType::SEMI || type == JoinType::RIGHT_SEMI) {
				tables.semi_senders.insert(child.get());
			}
		}
		BindIntermediateOutputs(child, root, optimizer, tables, below_join, child_can_receive);
		if (!below_join || !tables.NeedsResultBinding(*child) ||
		    op->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			continue;
		}
		auto old_bindings = child->GetColumnBindings();
		child = LogicalProjection::CreateIdentity(optimizer.binder.GenerateTableIndex(), std::move(child));
		child->ResolveOperatorTypes();
		ColumnBindingReplacer replacer;
		replacer.AddReplacements(old_bindings, child->GetColumnBindings());
		replacer.stop_operator = child.get();
		replacer.VisitOperator(root);
	}
}

unique_ptr<LogicalOperator> PredicateTransferOptimizer::PreOptimize(unique_ptr<LogicalOperator> plan) {
	graph_manager.table_operator_manager.semi_senders.clear();
	BindIntermediateOutputs(plan, *plan, optimizer, graph_manager.table_operator_manager);
	graph_manager.Build(*plan);
	return plan;
}

unique_ptr<LogicalOperator> PredicateTransferOptimizer::Optimize(unique_ptr<LogicalOperator> plan) {
	auto &ordered_nodes = graph_manager.transfer_order;
	if (!CheckIfGeneratedRPT(*plan)) {
		// **Forward pass**: Process nodes in reverse order (from last to first)
		// - Generate Bloom Filters (BFs) based on predicates
		// - Add BFs to the corresponding edges in the graph
		auto steps = GenerateNodeOrderPlan();
		final_transfer_steps = steps;
		auto modification = CreateModificationMap(steps);

		plan = InsertTransferOperators(std::move(plan), modification);
		// std::cerr << plan->ToString() << std::endl;
	}

	if (config.use_table_filter_bf) {
		plan = RPTPushUseBFIntoTable(optimizer).Optimize(std::move(plan));
	}

	plan = RPTReorderMarkJoin(optimizer).Optimize(std::move(plan));

	if (config.late_materialize_flag) {
		plan = RPTLateMaterialization(*this, config.use_row_id_filter).Optimize(std::move(plan));
		RemoveUnusedColumns(optimizer).VisitOperator(plan);
		ColumnLifetimeAnalyzer(optimizer, *plan, true).VisitOperator(*plan);	
		UpdateBindings(*plan);
	}
	

	TransferBFLinker().LinkBFOperators(*plan);
	return plan;
}

void PredicateTransferOptimizer::UpdateBindings(LogicalOperator& op) {
	if (TableOperatorManager::CheckLogicalCreateBF(op)) {
		op.Cast<LogicalCreateBF>().UpdateBindings();
	} else if (TableOperatorManager::CheckLogicalUseBF(op)) {
		op.Cast<LogicalUseBF>().UpdateBindings();
	}
	for (auto &child : op.children) {
		UpdateBindings(*child);
	}
}


unique_ptr<LogicalOperator> PredicateTransferOptimizer::InsertTransferOperators(unique_ptr<LogicalOperator> plan, unordered_map<LogicalOperator *, unique_ptr<LogicalOperator>>& modification) {
	for (auto &child : plan->children) {
		child = InsertTransferOperators(std::move(child), modification);
	}

	// Store original operator pointer
	LogicalOperator *original_operator = plan.get();
	auto apply_modification = [&](unordered_map<LogicalOperator *, unique_ptr<LogicalOperator>> &modify_map) {
		auto it = modify_map.find(original_operator);
		if (it == modify_map.end()) {
			// No modification needed
			return;
		}

		auto *last_op = it->second.get();
		auto *last_creator = (TableOperatorManager::CheckLogicalCreateBF(*it->second)) ? it->second.get() : nullptr;
		while (!last_op->children.empty()) {
			last_op = last_op->children[0].get();
			if (TableOperatorManager::CheckLogicalCreateBF(*last_op)) {
				last_creator = last_op;
			}
		}

		// table scan
		if (last_creator == last_op || plan->type != LogicalOperatorType::LOGICAL_FILTER) {
			last_op->AddChild(std::move(plan));
			plan = std::move(it->second);
			return;
		}

		// in clause
		auto &filter = plan->Cast<LogicalFilter>();
		if (!filter.expressions.empty() &&
		    filter.expressions[0]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
			last_op->AddChild(std::move(plan));
			plan = std::move(it->second);
			return;
		}

		// creator ---> user --> creator --> filter --> user --> user --> scan
		last_op->AddChild(std::move(plan->children[0]));
		if (last_creator == nullptr) {
			plan->children[0] = std::move(it->second);
		} else {
			plan->children[0] = std::move(last_creator->children[0]);
			last_creator->children[0] = std::move(plan);
			plan = std::move(it->second);
		}
	};

	apply_modification(modification);

	return plan;
}

unordered_map<LogicalOperator*, unique_ptr<LogicalOperator>> PredicateTransferOptimizer::CreateModificationMap(const vector<PredicateTransferOptimizer::TransferStep>& steps) {
	
	std::unordered_map<LogicalOperator *, unique_ptr<LogicalOperator>> modification;

	for (auto& step : steps) {
		auto& op = modification[step.table];
		if (!op) {
			op = BuildOpFromStep(step);
		} else {
			auto new_op = BuildOpFromStep(step);
			LogicalOperator* last = new_op.get();
			while (!last->children.empty()) {
				last = last->children[0].get();
			}
			last->AddChild(std::move(op));
			op = std::move(new_op);
		}
	}

	return modification;
}

vector<PredicateTransferOptimizer::TransferStep> PredicateTransferOptimizer::GenerateNodeOrderPlan() {
	vector<PredicateTransferOptimizer::TransferStep> steps;
	unordered_map<idx_t, vector<shared_ptr<FilterPlan>>> bf_to_use_map;

	for (auto& transfer_step : graph_manager.result_transfer_steps) {
		auto step = CreateBloomFilterPlanOnTransferStep(transfer_step, bf_to_use_map);
		if (step.table && (!step.create_bf.empty() || !step.use_bf.empty())) {
			steps.emplace_back(std::move(step));
		}
	}

	for (auto& data_pair : bf_to_use_map) {
		auto& filter_plans = data_pair.second;
		if (filter_plans.empty()) {
			continue;
		}
		LogicalOperator* table = graph_manager.table_operator_manager.GetTableOperator(data_pair.first);
		PredicateTransferOptimizer::TransferStep step;
		step.table = table;
		step.use_bf = filter_plans;
		steps.push_back(std::move(step));
	}

	return steps;
}

PredicateTransferOptimizer::TransferStep PredicateTransferOptimizer::CreateBloomFilterPlanOnTransferStep(const TransferGraphManager::TransferStep &step, unordered_map<idx_t, vector<shared_ptr<FilterPlan>>>& bf_to_use_map) {
	
	PredicateTransferOptimizer::TransferStep result;
	idx_t node_id = TableOperatorManager::GetScalarTableIndex(*step.table);
	result.table = step.table;
	GetAllBFsToUse(node_id, result.use_bf, bf_to_use_map);
	GetAllBFsToCreate(node_id, result.create_bf, bf_to_use_map, step.create_bf);

	return result;
}


void PredicateTransferOptimizer::GetAllBFsToUse(idx_t cur_node_id, vector<shared_ptr<FilterPlan>> &bfs_to_use_plan, unordered_map<idx_t, vector<shared_ptr<FilterPlan>>>& bf_to_use_map) const {
	for (auto &bf : bf_to_use_map[cur_node_id]) {
		if (std::find(bfs_to_use_plan.begin(), bfs_to_use_plan.end(), bf) == bfs_to_use_plan.end()) {
			bfs_to_use_plan.emplace_back(bf);
		}
	}
	// suppose we do not need to apply one filter twice
	bf_to_use_map[cur_node_id].clear();
}

void PredicateTransferOptimizer::GetAllBFsToCreate(idx_t cur_node_id,
                                                   vector<shared_ptr<FilterPlan>> &bfs_to_create_plan, unordered_map<idx_t, vector<shared_ptr<FilterPlan>>>& bf_to_use_map, const vector<shared_ptr<GraphEdge>>& edges) {
	for (auto &edge : edges) {
		auto bf_plan = make_shared_ptr<FilterPlan>();
		bf_plan->plan_id = global_filter_plan_id;
		global_filter_plan_id += 1;

		// Each expression leads to a bloom filter on a column on this table
		idx_t size = edge->left.size();
		for (idx_t i = 0; i < size; ++i) {
			auto &left_binding = edge->left[i];
			auto &right_binding = edge->right[i];
			auto &return_type = edge->return_types[i];

			bf_plan->return_types.push_back(return_type);

			auto binding0 = graph_manager.table_operator_manager.GetRenaming(left_binding);
			auto binding1 = graph_manager.table_operator_manager.GetRenaming(right_binding);

			if (binding0.table_index.index == cur_node_id) {
				bf_plan->build.push_back(binding0);
				bf_plan->apply.push_back(binding1);
			} else if (binding1.table_index.index == cur_node_id) {
				bf_plan->build.push_back(binding1);
				bf_plan->apply.push_back(binding0);
			} else {
				throw InternalException("Invalid edge!");
			}
		}
		if (!bf_plan->build.empty()) {
			// check if duplicated plan
			bool exist = false;
			for (auto& plan : bf_to_use_map[edge->destination]) {
				if (*plan == *bf_plan) {
					exist = true;
					break;
				}
			}
			if (!exist) {
				bfs_to_create_plan.emplace_back(bf_plan);
				bf_to_use_map[edge->destination].emplace_back(bf_plan);	
			}
		} else {
			throw InternalException("No built column found!");
		}
	}
}

unique_ptr<LogicalOperator> PredicateTransferOptimizer::BuildOpFromStep(const PredicateTransferOptimizer::TransferStep& step) const {
	if (!step.table || (step.create_bf.empty() && step.use_bf.empty())) {
		return nullptr;
	}

	if (!step.create_bf.empty() && !step.use_bf.empty()) {
		auto last_use_bf = BuildUseBFOperator(*step.table, step.use_bf);
		auto create_bf = BuildCreateBFOperator(*step.table, step.create_bf);
		create_bf->AddChild(unique_ptr_cast<LogicalUseBF, LogicalOperator>(std::move(last_use_bf)));
		return unique_ptr_cast<LogicalCreateBF, LogicalOperator>(std::move(create_bf));
	} else if (!step.use_bf.empty()) {
		auto last_use_bf = BuildUseBFOperator(*step.table, step.use_bf);
		return unique_ptr_cast<LogicalUseBF, LogicalOperator>(std::move(last_use_bf));
	} else {
		auto create_bf = BuildCreateBFOperator(*step.table, step.create_bf);
		return unique_ptr_cast<LogicalCreateBF, LogicalOperator>(std::move(create_bf));
	}
}

unique_ptr<LogicalCreateBF>
PredicateTransferOptimizer::BuildCreateBFOperator(const LogicalOperator &node, const vector<shared_ptr<FilterPlan>> &bf_plans) const {
	auto create_bf = make_uniq<LogicalCreateBF>(bf_plans);
	create_bf->use_dynamic_in_filter = config.use_dynamic_in_filter;
	create_bf->use_bitmap_filter = config.use_bitmap_filter;
	create_bf->use_table_filter_bf = config.use_table_filter_bf;
	create_bf->SetEstimatedCardinality(node.estimated_cardinality);
	return create_bf;
}

unique_ptr<LogicalUseBF> PredicateTransferOptimizer::BuildUseBFOperator(const LogicalOperator &node,
                                                                        const vector<shared_ptr<FilterPlan>> &bf_plans) const {
	unique_ptr<LogicalUseBF> last_operator;

	// This is important for performance, not use (int i = 0; i < temp_result_to_use.size(); i++)
	for (auto it = bf_plans.rbegin(); it != bf_plans.rend(); ++it) {
		auto use_bf_operator = make_uniq<LogicalUseBF>(*it);
		use_bf_operator->SetEstimatedCardinality(node.estimated_cardinality);
		if (last_operator) {
			use_bf_operator->AddChild(std::move(last_operator));
		}
		last_operator = std::move(use_bf_operator);
	}

	return last_operator;
}

bool PredicateTransferOptimizer::CheckIfGeneratedRPT(const LogicalOperator &node) {
	if (node.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		if (TableOperatorManager::CheckLogicalCreateBF(node) || TableOperatorManager::CheckLogicalUseBF(node)) {
			return true;
		}
	}
	for (auto &child : node.children) {
		if (child) {
			if (CheckIfGeneratedRPT(*child)) {
				return true;
			}
		}
	}
	return false;
}

void PredicateTransferOptimizer::GetColumnBindingExpression(Expression &expr,
                                                            vector<BoundColumnRefExpression *> &expressions) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		Expression *expr_ptr = &expr;
		BoundColumnRefExpression *col_ref = static_cast<BoundColumnRefExpression *>(expr_ptr);
		D_ASSERT(col_ref->Depth() == 0);
		expressions.emplace_back(col_ref);
	} else {
		ExpressionIterator::EnumerateChildren(
		    expr, [&](unique_ptr<Expression> &child) { GetColumnBindingExpression(*child, expressions); });
	}
}

string PredicateTransferOptimizer::GetDebugInfo() const {
	stringstream ss;
	ss << "tables=" << graph_manager.TablesToString() << "\n";
	ss << "edges=" << graph_manager.EdgesToString() << "\n";
	ss << "transfer_plan=" << graph_manager.TransferPlanToString() << "\n";
	ss << "transfer_steps=(";
	for (auto& step : final_transfer_steps) {
		ss << step.ToString() << "\n";
	}
	ss << ")\n";
	return ss.str();
}

string PredicateTransferOptimizer::TransferStep::ToString() const {
	stringstream ss;
	ss << "((table=" << TableOperatorManager::GetTableName(*table) << ", table_id=" << TableOperatorManager::GetScalarTableIndex(*table) << ")\n";
	if (!use_bf.empty()) {
		ss << "use_bf=(";
		for (auto& filter_plan : use_bf) {
			ss << filter_plan->ToString() << "\n";
		}
	}
	if (!create_bf.empty()) {
		ss << "create_bf=(";
		for (auto& filter_plan : create_bf) {
			ss << filter_plan->ToString() << "\n";
		}	
	}
	
	ss << ")";
	return ss.str();
}

} // namespace duckdb