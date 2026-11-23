#include "predicate_transfer/join_graph/table_operator_manager.hpp"

#include "predicate_transfer/join_graph/key_column.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"

namespace duckdb::predicate_transfer_graph {

void TableOperatorManager::Build(LogicalOperator &plan) {
	ClearTableOperators();
	rename_col_bindings.clear();
	CollectJoins(plan);
	CollectTableOperators(plan);
}

LogicalOperator *TableOperatorManager::GetTableOperator(idx_t table_idx) {
	auto itr = table_operators.find(table_idx);
	if (itr == table_operators.end()) {
		return nullptr;
	}

	return &itr->second.get();
}

const LogicalOperator *TableOperatorManager::GetTableOperator(idx_t table_idx) const {
	auto itr = table_operators.find(table_idx);
	if (itr == table_operators.end()) {
		return nullptr;
	}

	return &itr->second.get();
}

optional_ptr<LogicalOperator> TableOperatorManager::GetCTEDefinition(LogicalOperator &scan, ColumnBinding key, ColumnBinding &output) {
	auto &ref = scan.Cast<LogicalCTERef>();
	auto entry = cte_definitions.find(ref.cte_index.index);
	if (entry == cte_definitions.end() || ref.table_index != key.table_index) {
		return nullptr;
	}
	auto &definition = entry->second.get();
	const auto cols = definition.GetColumnBindings();
	if (key.column_index >= cols.size() || cols.size() != scan.GetColumnBindings().size()) {
		return nullptr;
	}
	output = cols[key.column_index];
	return &definition;
}

bool TableOperatorManager::CanScheduleTransfer(idx_t source, idx_t target) const {
	// A CTE reader can only be scheduled after its producer has registered
	// the scan pipelines. An outer pipeline cannot pull it in as a BF builder.
	auto required = table_scopes.find(source);
	if (required == table_scopes.end()) {
		return true;
	}
	auto available = table_scopes.find(target);
	if (available == table_scopes.end()) {
		return false;
	}
	for (auto index : required->second) {
		if (!available->second.count(index)) {
			return false;
		}
	}
	return true;
}

ColumnBinding TableOperatorManager::GetRenaming(ColumnBinding binding) {
	auto itr = rename_col_bindings.find(binding);
	while (itr != rename_col_bindings.end()) {
		binding = itr->second;
		itr = rename_col_bindings.find(binding);
	}
	return binding;
}

string TableOperatorManager::GetTableName(const LogicalOperator &op) {
	string ret;
	auto params = op.ParamsToString();

	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		if (params.contains("Table")) {
			ret = params.at("Table");
		} else {
			ret = "Unknown";
		}
		return ret;
	}

	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		return "CTE_SCAN " + to_string(op.Cast<LogicalCTERef>().cte_index.index);
	}

	if (op.type == LogicalOperatorType::LOGICAL_CHUNK_GET) {
		return "MemoryScan";
	}

	if (params.contains("Table")) {
		ret = params.at("Table");
	} else if (op.children.size() != 1) {
		ret = "Unknown";
	} else {
		ret = GetTableName(*op.children[0]);
	}

	return ret;
}

string TableOperatorManager::GetColumnName(const ColumnBinding &binding) {
	auto origin = cte_origins.find(binding);
	if (origin != cte_origins.end()) {
		return GetColumnName(GetRenaming(origin->second));
	}
	auto op = GetTableOperator(binding.table_index.index);

	// If the table operator was wrapped in a filter, unwrap it to find the base table names
	while (op && op->type == LogicalOperatorType::LOGICAL_FILTER && !op->children.empty()) {
		op = op->children[0].get();
	}

	if (op && op->type == LogicalOperatorType::LOGICAL_GET) {
		auto &get = op->Cast<LogicalGet>();
		if (binding.column_index < get.GetColumnIds().size()) {
			auto &col_idx = get.GetColumnIds()[binding.column_index];
			if (col_idx.IsRowIdColumn()) {
				return "rowid";
			}
			if (col_idx.HasPrimaryIndex()) {
				auto idx = col_idx.GetPrimaryIndex();
				if (idx < get.names.size()) {
					return get.names[idx].GetIdentifierName();
				}
			} else {
				return col_idx.GetFieldName();
			}
		}
	}

	return "unknown_column_" + std::to_string(binding.column_index);
}

// Follow only unchanged values through a row-preserving join input or an ordinary group key.
bool TableOperatorManager::ResolveScanKey(LogicalOperator &op, ColumnBinding &key, const LogicalType &type,
                           TableOperatorManager *producers) {
	auto bindings = op.GetColumnBindings();
	auto it = std::find(bindings.begin(), bindings.end(), key);
	if (it == bindings.end()) {
		return false;
	}
	auto offset = static_cast<idx_t>(it - bindings.begin());
	const Expression *expr = nullptr;
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		return offset < op.types.size() && op.types[offset] == type;
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
		return producers && ResolveScanKey(*op.children[1], key, type, producers);
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		ColumnBinding output_key;
		auto definition = producers ? producers->GetCTEDefinition(op, key, output_key) : nullptr;
		if (!definition || !ResolveScanKey(*definition, output_key, type, producers)) {
			return false;
		}
		key = output_key;
		return true;
	}
	case LogicalOperatorType::LOGICAL_WINDOW:
		if (!producers) {
			return false;
		}
		return ResolveScanKey(*op.children[0], key, type, producers);
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		auto &distinct = op.Cast<LogicalDistinct>();
		if (distinct.distinct_type != DistinctType::DISTINCT ||
		    std::none_of(distinct.distinct_targets.begin(), distinct.distinct_targets.end(), [&](const unique_ptr<Expression> &target) {
			    return target->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
			           target->Cast<BoundColumnRefExpression>().Binding() == key;
		    })) {
			return false;
		}
		return ResolveScanKey(*op.children[0], key, type, producers);
	}
	case LogicalOperatorType::LOGICAL_UNNEST:
		// Only values carried from the input are present in the child's bindings.
		return ResolveScanKey(*op.children[0], key, type, producers);
	case LogicalOperatorType::LOGICAL_FILTER:
		return ResolveScanKey(*op.children[0], key, type, producers);
	case LogicalOperatorType::LOGICAL_PROJECTION:
		expr = op.expressions[offset].get();
		break;
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &agg = op.Cast<LogicalAggregate>();
		if (key.table_index != agg.group_index || offset >= agg.groups.size() || agg.grouping_sets.size() > 1 ||
		    (!agg.grouping_sets.empty() && agg.grouping_sets[0].size() != agg.groups.size())) {
			return false;
		}
		expr = agg.groups[offset].get();
		break;
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto join_type = op.Cast<LogicalComparisonJoin>().join_type;
		for (idx_t side = 0; side < 2; side++) {
			const bool single_input = producers && join_type == JoinType::SINGLE && side == 0;
			if (!single_input && join_type != JoinType::INNER && join_type != (side == 0 ? JoinType::LEFT : JoinType::RIGHT)) {
				continue;
			}
			auto input_key = key;
			if (ResolveScanKey(*op.children[side], input_key, type, producers)) {
				key = input_key;
				return true;
			}
		}
		return false;
	}
	default:
		return false;
	}
	if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return false;
	}
	key = expr->Cast<BoundColumnRefExpression>().Binding();
	return ResolveScanKey(*op.children[0], key, type, producers);
}

bool TableOperatorManager::NeedsResultBinding(const LogicalOperator &op) const {
	if (semi_senders.count(&op)) {
		return true;
	}
	// Filtering the input of a limit or sample can change which rows it selects.
	return op.type == LogicalOperatorType::LOGICAL_WINDOW || op.type == LogicalOperatorType::LOGICAL_LIMIT ||
	       op.type == LogicalOperatorType::LOGICAL_TOP_N || op.type == LogicalOperatorType::LOGICAL_SAMPLE ||
	       (op.type == LogicalOperatorType::LOGICAL_FILTER &&
	        op.children[0]->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY);
}

idx_t TableOperatorManager::GetScalarTableIndex(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_CTE_REF:
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_DELIM_GET:
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		return op.GetTableIndex()[0].index;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		return GetScalarTableIndex(*op.children[0]);
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		return op.GetTableIndex()[1].index;
	}
	default: {
		// For joins folded into a FILTER (e.g. MARK JOIN for IN),
		// traverse the left child to find the base table index
		if (op.children.empty()) {
			return std::numeric_limits<idx_t>::max();
		}
		return GetScalarTableIndex(*op.children[0]);
	}
	}
}

void TableOperatorManager::AddTableOperator(LogicalOperator &op) {
	op.estimated_cardinality = op.EstimateCardinality(context);
	table_dfs_order.push_back(op);

	idx_t table_idx = GetScalarTableIndex(op);
	if (table_idx != std::numeric_limits<idx_t>::max() && table_operators.find(table_idx) == table_operators.end()) {
		table_operators.emplace(table_idx, op);
		if (!active_scopes.empty()) {
			table_scopes.emplace(table_idx, active_scopes);
		}
	}
}

//===--------------------------------------------------------------------===//
// Two-pass extraction
//===--------------------------------------------------------------------===//
//
// Conceptual model: PT cuts the plan tree at every inner equi-join boundary.
// The children of those joins are exactly the table operators; everything
// between joins (FILTER, PROJECTION, AGG, CROSS_PRODUCT, MARK JOIN, …) is
// just *part of* the table operator subtree on that side.
//
//   Pass 1 — CollectJoins: walk the tree, collect inner equi-joins into
//     join_operators, and mark every operator whose subtree contains at
//     least one inner equi-join in contains_join_set_.
//
//   Pass 2 — CollectTableOperators: any subtree NOT in contains_join_set_
//     becomes a table operator (admitted at its root). When descending
//     through contains_join_set_ operators, record PROJECTION/AGG renames
//     so upstream join conditions can resolve through to the admitted
//     subtree's table_index.
//
bool TableOperatorManager::IsInnerEquiJoin(const LogicalOperator &op) {
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN && op.type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		return false;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	switch (join.join_type) {
	case JoinType::INNER:
	case JoinType::LEFT:
	case JoinType::RIGHT:
	case JoinType::SEMI:
	case JoinType::RIGHT_SEMI:
		break;
	default:
		return false;
	}
	return std::any_of(join.conditions.begin(), join.conditions.end(), [](const JoinCondition &jc) {
		return jc.IsComparison() && jc.GetComparisonType() == ExpressionType::COMPARE_EQUAL &&
		       jc.GetLHS().GetExpressionType() == ExpressionType::BOUND_COLUMN_REF &&
		       jc.GetRHS().GetExpressionType() == ExpressionType::BOUND_COLUMN_REF;
	});
}

bool TableOperatorManager::CollectJoins(LogicalOperator &op) {
	// A CTE reference is a leaf. Its owner supplies output bindings in pass 2.
	if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return true;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		return false;
	}
	// Descend through these boundaries to retain joins inside each input.
	// Pass 2 can independently register the completed output for an outer join.
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_LIMIT:
	case LogicalOperatorType::LOGICAL_TOP_N:
	case LogicalOperatorType::LOGICAL_SAMPLE:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		for (auto &child : op.children) {
			CollectJoins(*child);
		}
		contains_join_set_.insert(&op);
		return true; // force pass 2 to descend through us
	}
	default:
		break;
	}

	// LOGICAL_DELIM_GET is a *descent boundary*, not a join. The cardinality
	// oracle cannot execute a subtree containing a DELIM_GET descendant
	// standalone — PhysicalColumnDataScan(DELIM_SCAN) requires its parent
	// PhysicalDelimJoin to register a delim_join_dependency, and that parent
	// is not part of the copied subtree. So we must descend down to the
	// DELIM_GET itself and admit it as its own table operator (matching the
	// historical behavior of the old extractor). Returning true here marks
	// every ancestor as needing descent in pass 2.
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return true;
	}

	bool subtree_has_boundary = op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
	if (IsInnerEquiJoin(op)) {
		join_operators.push_back(op);
		for (const auto &condition : op.Cast<LogicalComparisonJoin>().conditions) {
			if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
				continue;
			}
			for (auto expr : {&condition.GetLHS(), &condition.GetRHS()}) {
				if (expr->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
					join_keys.insert(expr->Cast<BoundColumnRefExpression>().Binding());
				}
			}
		}
		subtree_has_boundary = true;
	}
	for (auto &child : op.children) {
		if (CollectJoins(*child)) {
			subtree_has_boundary = true;
		}
	}
	if (subtree_has_boundary) {
		contains_join_set_.insert(&op);
	}
	return subtree_has_boundary;
}

void TableOperatorManager::RecordRenameOneLevel(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto old_refs = op.GetColumnBindings();
		for (size_t i = 0; i < op.expressions.size(); i++) {
			auto ref = FindKeyColumn(*op.expressions[i]);
			if (ref) {
				rename_col_bindings.emplace(old_refs[i], ref->Binding());
				if (join_keys.count(old_refs[i])) {
					join_keys.insert(ref->Binding());
				}
			}
		}
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &agg = op.Cast<LogicalAggregate>();
		if (agg.groups.empty()) {
			return;
		}
		auto old_refs = agg.GetColumnBindings();
		for (size_t i = 0; i < agg.groups.size(); i++) {
			if (agg.groups[i]->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
				auto &col_ref = agg.groups[i]->Cast<BoundColumnRefExpression>();
				rename_col_bindings.insert({old_refs[i], col_ref.Binding()});
				if (join_keys.count(old_refs[i])) {
					join_keys.insert(col_ref.Binding());
				}
			}
		}
	}
}

static bool IsTransparentForTableOp(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		return true;
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &agg = op.Cast<LogicalAggregate>();
		return !agg.groups.empty();
	}
	if (op.type == LogicalOperatorType::LOGICAL_ANY_JOIN) {
		// Input keys cover every matching pair. Keep the theta predicate in
		// the join and use its inputs for transfers on other equality edges.
		auto &join = op.Cast<LogicalAnyJoin>();
		return join.join_type == JoinType::INNER && !join.condition->IsVolatile() && !join.condition->CanThrow();
	}
	return false;
}

void TableOperatorManager::CollectTableOperators(LogicalOperator &op) {
	// A dependent scan needs its owning delim join to register the pipeline.
	// A filter built inside that branch can only feed consumers in the same scope.
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_JOIN) {
		const idx_t dependent = op.Cast<LogicalComparisonJoin>().delim_flipped ? 0 : 1;
		CollectTableOperators(*op.children[1 - dependent]);
		active_scopes.insert(&op);
		CollectTableOperators(*op.children[dependent]);
		active_scopes.erase(&op);
		return;
	}
	// A materialized CTE can be scanned more than once. Keep each consumer's
	// binding for physical transfer; a filter on one scan cannot prune siblings.
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		CollectTableOperators(*op.children[0]);
		const auto index = op.Cast<LogicalMaterializedCTE>().table_index.index;
		cte_definitions.emplace(index, *op.children[0]);
		cte_outputs.emplace(index, op.children[0]->GetColumnBindings());
		active_scopes.insert(&op);
		CollectTableOperators(*op.children[1]);
		active_scopes.erase(&op);
		cte_outputs.erase(index);
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		const auto &ref = op.Cast<LogicalCTERef>();
		auto producer = cte_outputs.find(ref.cte_index.index);
		if (producer != cte_outputs.end()) {
			auto bindings = op.GetColumnBindings();
			if (bindings.size() == producer->second.size()) {
				AddTableOperator(op);
				for (idx_t i = 0; i < bindings.size(); i++) {
					cte_origins.emplace(bindings[i], producer->second[i]);
				}
			}
		}
		return;
	}
	// Build from surviving recursive rows when a filter bounds the completed result.
	auto *result = &op;
	while (result->type == LogicalOperatorType::LOGICAL_FILTER) {
		result = result->children[0].get();
	}
	if (result->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		const auto bindings = op.GetColumnBindings();
		if (std::any_of(bindings.begin(), bindings.end(), [&](const ColumnBinding &b) { return join_keys.count(b); })) {
			AddTableOperator(op);
		}
		return;
	}
	// DELIM_GET cannot be executed standalone via the cardinality oracle —
	// PhysicalColumnDataScan(DELIM_SCAN) requires its parent PhysicalDelimJoin
	// to register a delim_join_dependency. Skip it entirely. The correlated
	// subquery's delim side carries no useful filter information for PT.
	if (op.type == LogicalOperatorType::LOGICAL_DELIM_GET) {
		return;
	}
	// Transparent wrappers (PROJECTION, grouped AGG): never admit as a table
	// operator root. Record their renames so upstream join conditions can
	// resolve through to the deeper subtree's bindings, then descend.
	// These outputs define their own key domain. Do not rename a set key
	// into a branch or a window key into the input on which it was computed.
	const bool intermediate_output = op.type == LogicalOperatorType::LOGICAL_PROJECTION &&
	                                 NeedsResultBinding(*op.children[0]);
	const bool set_output = op.type == LogicalOperatorType::LOGICAL_UNION ||
	                        op.type == LogicalOperatorType::LOGICAL_EXCEPT ||
	                        op.type == LogicalOperatorType::LOGICAL_INTERSECT;
	const bool scalar_aggregate = op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY &&
	                              op.Cast<LogicalAggregate>().groups.empty();
	if (intermediate_output || set_output || scalar_aggregate) {
		const auto bindings = op.GetColumnBindings();
		if (std::any_of(bindings.begin(), bindings.end(), [&](const ColumnBinding &b) { return join_keys.count(b); })) {
			AddTableOperator(op);
		}
		const bool having_output = intermediate_output && op.children[0]->type == LogicalOperatorType::LOGICAL_FILTER;
		if (having_output) {
			// Register the grouping input before resolving unchanged group keys.
			// The projection above HAVING supplies the completed aggregate values.
			CollectTableOperators(*op.children[0]->children[0]);
			for (idx_t i = 0; i < bindings.size(); i++) {
				auto key = bindings[i];
				if (join_keys.count(key) && ResolveScanKey(op, key, op.types[i]) &&
				    GetTableOperator(key.table_index.index)) {
					rename_col_bindings.emplace(bindings[i], key);
				}
			}
		} else {
			for (auto &child : op.children) {
				CollectTableOperators(*child);
			}
		}
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		// Keep a computed join key at the projection that evaluates it. Using
		// its input column could hash a different value or hide a cast error.
		const auto bindings = op.GetColumnBindings();
		for (idx_t i = 0; i < bindings.size(); i++) {
			const auto &expr = *op.expressions[i];
			if (!join_keys.count(bindings[i])) {
				continue;
			}
			// Preserve the consumer's hash key type across a CTE boundary;
			// its producer may store a different representation.
			if (!FindKeyColumn(expr) || (!cte_outputs.empty() && BoundCastExpression::IsCast(expr))) {
				AddTableOperator(op);
				if (contains_join_set_.count(&op)) {
					CollectTableOperators(*op.children[0]);
				}
				return;
			}
		}
	}
	if (IsTransparentForTableOp(op)) {
		RecordRenameOneLevel(op);
		for (auto &child : op.children) {
			CollectTableOperators(*child);
		}
		return;
	}
	if (op.type == LogicalOperatorType::LOGICAL_DISTINCT && !contains_join_set_.count(&op)) {
		// A scan key can filter before ordinary duplicate elimination. Keep
		// the result binding when any requested key is computed by the subtree.
		auto scan = &op;
		while (scan->children.size() == 1) {
			scan = scan->children[0].get();
		}
		if (scan->type == LogicalOperatorType::LOGICAL_GET) {
			const auto bindings = op.GetColumnBindings();
			vector<pair<ColumnBinding, ColumnBinding>> resolved;
			for (idx_t i = 0; i < bindings.size(); i++) {
				auto key = bindings[i];
				if (!join_keys.count(key)) {
					continue;
				}
				if (!ResolveScanKey(op, key, op.types[i])) {
					resolved.clear();
					break;
				}
				resolved.emplace_back(bindings[i], key);
			}
			if (!resolved.empty()) {
				for (auto &entry : resolved) {
					if (entry.first != entry.second) {
						rename_col_bindings.emplace(entry.first, entry.second);
					}
				}
				AddTableOperator(*scan);
				return;
			}
		}
	}
	if (!contains_join_set_.count(&op)) {
		// Maximal subtree without an inner equi-join, rooted at a non-
		// transparent operator (FILTER, GET, CHUNK_GET, CROSS_PRODUCT, MARK
		// JOIN, OUTER JOIN, …). Admit it.
		AddTableOperator(op);
		return;
	}
	// Subtree contains inner equi-join(s); descend.
	for (auto &child : op.children) {
		CollectTableOperators(*child);
	}
}

} // namespace duckdb::predicate_transfer_graph
