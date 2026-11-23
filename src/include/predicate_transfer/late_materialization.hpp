#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/common/table_column.hpp"

namespace duckdb {
class LogicalOperator;
class LogicalGet;
class LogicalLimit;
class PredicateTransferOptimizer;

class RPTLateMaterialization : public BaseColumnPruner {
public:
	explicit RPTLateMaterialization(PredicateTransferOptimizer &optimizer, bool use_row_id_filter);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	bool TryLateMaterialization(unique_ptr<LogicalOperator> &op);
	vector<idx_t> GetOrInsertRowIds(LogicalGet &get);

	pair<unique_ptr<LogicalOperator>, reference<LogicalGet>>
	ConstructLHS(LogicalGet &get, const vector<reference<LogicalOperator>> &filters);

	bool CheckIfHeavyFilter(const LogicalGet& get);
	void CreateRowIDFilter(const LogicalGet& get, LogicalGet& new_get, LogicalOperator& dynamic_select);

private:
	PredicateTransferOptimizer &optimizer;
	//! The row-id column ids
	vector<column_t> row_id_column_ids;
	//! The row-id columns
	vector<TableColumn> row_id_columns;
	vector<idx_t> row_id_indexes;
	bool use_row_id_filter = false;
};

} // namespace duckdb
