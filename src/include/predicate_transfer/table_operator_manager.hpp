#pragma once

#include "predicate_transfer/join_graph/table_operator_manager.hpp"

namespace duckdb {

//! RPT-only helpers for recognizing transfer operators after plan rewriting.
class TableOperatorManager : public predicate_transfer_graph::TableOperatorManager {
public:
	static bool CheckLogicalCreateBF(const LogicalOperator &op);
	static bool CheckLogicalUseBF(const LogicalOperator &op);
};

} // namespace duckdb
