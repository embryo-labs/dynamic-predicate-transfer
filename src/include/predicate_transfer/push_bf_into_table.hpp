#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {
class LogicalOperator;
class LogicalGet;
class LogicalLimit;
class Optimizer;

class RPTPushUseBFIntoTable {
public:
	explicit RPTPushUseBFIntoTable(Optimizer &optimizer);

	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);

private:
	void TryOptimize(unique_ptr<LogicalOperator> &op);

private:
	Optimizer &optimizer;
};

} // namespace duckdb
