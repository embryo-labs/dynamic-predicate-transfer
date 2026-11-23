#include "predicate_transfer/late_materialization.hpp"
#include "predicate_transfer/table_operator_manager.hpp"
#include "predicate_transfer/operator/logical_dynamic_select.hpp"
#include "predicate_transfer/dag.hpp"
#include "predicate_transfer/predicate_transfer_optimizer.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

RPTLateMaterialization::RPTLateMaterialization(PredicateTransferOptimizer &optimizer, bool use_row_id_filter) : optimizer(optimizer), use_row_id_filter(use_row_id_filter) {
}

unique_ptr<LogicalOperator> RPTLateMaterialization::Optimize(unique_ptr<LogicalOperator> op) {
	if (TableOperatorManager::CheckLogicalCreateBF(*op) || TableOperatorManager::CheckLogicalUseBF(*op)) {
		auto parent_references = column_references;
		if (TryLateMaterialization(op)) {
			column_references = parent_references;
			return op;
		}
		column_references = parent_references;
	}
	LogicalOperatorVisitor::VisitOperatorExpressions(*op);
	for (auto &child : op->children) {
		child = Optimize(std::move(child));
	}
	return op;
}

bool RPTLateMaterialization::TryLateMaterialization(unique_ptr<LogicalOperator> &op) {
	// save the column references except the subtree of op.
	auto parent_references = column_references;
	column_references.clear();

	// copy from duckdb::LateMaterialization
	vector<reference<LogicalOperator>> source_operators;
	vector<reference<LogicalOperator>> filters;
	VisitOperatorExpressions(*op);
	reference<LogicalOperator> child = *op->children[0];
	bool exist_create_bf = TableOperatorManager::CheckLogicalCreateBF(*op);
	while (child.get().type != LogicalOperatorType::LOGICAL_GET) {
		switch (child.get().type) {
		case LogicalOperatorType::LOGICAL_FILTER: {
			// visit filter expressions - we need these columns
			VisitOperatorExpressions(child.get());
			filters.push_back(child);
			// continue into child
			child = *child.get().children[0];
			break;
		}
		case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
			if (TableOperatorManager::CheckLogicalCreateBF(child.get()) || TableOperatorManager::CheckLogicalUseBF(child.get())) {
				exist_create_bf |= TableOperatorManager::CheckLogicalCreateBF(child.get());
				if (use_row_id_filter) {
					VisitOperatorExpressions(child.get());
					filters.push_back(child);
					child = *child.get().children[0];
					break;
				}
			}
			return false;
		}
		default:
			// unsupported operator for late materialization
			return false;
		}
	}
	// if there is no create bf, do not late materialization
	if (!exist_create_bf) {
		return false;
	}

	// check conditions
	auto &get = child.get().Cast<LogicalGet>();
	if (!get.function.late_materialization) {
		// this function does not support late materialization
		return false;
	}

	// check if there exists extra columns
	bool all_get_colid_exist_flag = true;
	for (auto col : get.GetColumnBindings()) {
		if (get.GetColumnType(get.GetColumnIds()[col.column_index]).IsNumeric()) {
			continue;
		}
		if (!column_references.count(col)) {
			all_get_colid_exist_flag = false;
			break;
		}
	}
	if (all_get_colid_exist_flag) {
		// we do not benefit from late materialization
		// all get columns are used in filters and create BF
		return false;
	}

	if (use_row_id_filter) {
		// get row id columns
		// copy from late materialization in DuckDB
		if (!get.function.get_row_id_columns) {
			throw InternalException("Function supports late materialization but not get_row_id_columns");
		}
		row_id_column_ids = get.function.get_row_id_columns(optimizer.GetOptimizer().context, get.bind_data.get());
		if (row_id_column_ids.empty()) {
			throw InternalException("Row Id Columns must not be empty");
		}
		row_id_columns.clear();
		for (auto &col_id : row_id_column_ids) {
			auto entry = get.virtual_columns.find(col_id);
			if (entry == get.virtual_columns.end()) {
				throw InternalException("Row id column id not found in virtual column list");
			}
			row_id_columns.push_back(entry->second);
		}

		// Now only support single integer row column
		if (row_id_columns.size() != 1 || !row_id_columns[0].type.IsIntegral()) {
			return false;
		}

		row_id_indexes = GetOrInsertRowIds(get);
	} else {
		// no row id filter -> no heavy filter
		if (CheckIfHeavyFilter(get)) {
			return false;
		}
	}

	std::reverse(filters.begin(), filters.end());
	auto lhs_pair = ConstructLHS(get, filters);
	auto &lhs = lhs_pair.first;
	auto new_table_index = lhs_pair.second.get().table_index;
	auto &new_get = lhs_pair.second.get();

	auto new_op = make_uniq<LogicalDynamicSelect>();
	for (auto &col : new_get.GetColumnBindings()) {
		auto type = new_get.GetColumnType(new_get.GetColumnIds()[col.column_index]);
		auto expr = make_uniq<BoundColumnRefExpression>(type, col);
		new_op->expressions.push_back(std::move(expr));
	}
	new_op->AddChild(std::move(lhs));
	new_op->AddChild(std::move(op));

	if (use_row_id_filter) {
		CreateRowIDFilter(get, new_get, *new_op);
	}

	op = std::move(new_op);

	// replace the table index in column bindings of parent operators to new table index
	for (auto &col : parent_references) {
		if (col.first.table_index == get.table_index) {
			for (auto &binding_expr : col.second.bindings) {
				if (binding_expr.get().BindingMutable().table_index == get.table_index) {
					binding_expr.get().BindingMutable().table_index = new_table_index;
				}
			}
		}
	}

	return true;
}

void RPTLateMaterialization::CreateRowIDFilter(const LogicalGet& get, LogicalGet& new_get, LogicalOperator& dynamic_select) {
	vector<ColumnBinding> build_cols, apply_cols;
	vector<LogicalType> ret_types;
	for (idx_t r_idx = 0; r_idx < row_id_column_ids.size(); ++r_idx) {
		auto &row_id_column = row_id_columns[r_idx];
		auto id = row_id_indexes[r_idx];
		build_cols.emplace_back(get.table_index, ProjectionIndex(id));
		apply_cols.emplace_back(new_get.table_index, ProjectionIndex(id));
		ret_types.emplace_back(row_id_column.type); // NOT USE get.types because get.types.size() may < get.column_ids.size(). (hidden col?)
	}

	// clear the filters in new_get
	new_get.table_filters = TableFilterSet();
	new_get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();

	// create a filter_plan for rowid
	auto filter_plan = make_shared_ptr<FilterPlan>();
	filter_plan->build = build_cols;
	filter_plan->apply = apply_cols;
	filter_plan->return_types = ret_types;
	filter_plan->plan_id = optimizer.GetNewFilterPlanID();
	filter_plan->exact_row_ids = true;

	// create create bf
	auto create_bf = optimizer.BuildCreateBFOperator(*dynamic_select.children[1], vector<shared_ptr<FilterPlan>>{filter_plan});
	create_bf->AddChild(std::move(dynamic_select.children[1]));
	create_bf->use_table_filter_bf = true;
	dynamic_select.children[1] = std::move(create_bf);

	// create use bf
	auto use_bf = optimizer.BuildUseBFOperator(new_get, vector<shared_ptr<FilterPlan>>{filter_plan});
	use_bf->AddChild(std::move(dynamic_select.children[0]));
	use_bf->use_table_filter_bf = true;
	dynamic_select.children[0] = std::move(use_bf);
}

vector<idx_t> RPTLateMaterialization::GetOrInsertRowIds(LogicalGet &get) {
	auto &column_ids = get.GetMutableColumnIds();

	vector<idx_t> result;
	for (idx_t r_idx = 0; r_idx < row_id_column_ids.size(); ++r_idx) {
		// check if it is already projected
		auto row_id_column_id = row_id_column_ids[r_idx];
		auto &row_id_column = row_id_columns[r_idx];
		optional_idx row_id_index;
		for (idx_t i = 0; i < column_ids.size(); ++i) {
			if (column_ids[i].GetPrimaryIndex() == row_id_column_id) {
				// already projected - return the id
				row_id_index = i;
				break;
			}
		}
		if (row_id_index.IsValid()) {
			result.push_back(row_id_index.GetIndex());
			continue;
		}
		// row id is not yet projected - push it and return the new index
		column_ids.push_back(ColumnIndex(row_id_column_id));
		if (!get.projection_ids.empty()) {
			get.projection_ids.emplace_back(column_ids.size() - 1);
		}
		if (!get.types.empty()) {
			get.types.push_back(row_id_column.type);
		}
		result.push_back(column_ids.size() - 1);
	}
	return result;
}

bool RPTLateMaterialization::CheckIfHeavyFilter(const LogicalGet& get) {
	for (auto &filter_pair : get.table_filters) {
		auto id = filter_pair.GetIndex();
		auto &filter = filter_pair.Filter();

		auto col_type = get.returned_types[id];
		if (!col_type.IsNumeric()) {
			return true;
		}
	}
	return false;
}

pair<unique_ptr<LogicalOperator>, reference<LogicalGet>>
RPTLateMaterialization::ConstructLHS(LogicalGet &get, const vector<reference<LogicalOperator>> &filters) {
	// we need to construct a new scan of the same table
	auto new_table_index = optimizer.GetOptimizer().binder.GenerateTableIndex();
	auto new_get = make_uniq<LogicalGet>(new_table_index, get.function, get.bind_data->Copy(), get.returned_types,
	                                     get.names, get.virtual_columns);
	new_get->GetMutableColumnIds() = get.GetColumnIds();
	new_get->projection_ids = get.projection_ids;
	// share one dynamic filter set between get and new_get
	if (!get.dynamic_filters) {
		get.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
	}
	new_get->dynamic_filters = get.dynamic_filters;
	new_get->table_filters = std::move(*get.table_filters.Copy());

	if (!filters.empty()) {
		auto &new_get_ref = *new_get;
		unique_ptr<LogicalOperator> ret = std::move(new_get);
		for (auto &filter : filters) {
			// TODO: copy only once. because this copies children.
			auto new_filter = filter.get().Copy(optimizer.GetOptimizer().context);
			new_filter->children[0] = std::move(ret);
			for (auto &expr : new_filter->expressions) {
				ExpressionIterator::EnumerateExpression(expr, [&get, new_table_index](unique_ptr<Expression> &child) {
					if (child->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
						return;
					}
					auto &binding = child->Cast<BoundColumnRefExpression>().BindingMutable();
					if (binding.table_index == get.table_index) {
						binding.table_index = new_table_index;
					}
				});
			}

			ret = std::move(new_filter);
		}
		return {std::move(ret), new_get_ref};
	} else {
		auto &new_get_ref = *new_get;
		return {std::move(new_get), new_get_ref};
	}
}

} // namespace duckdb
