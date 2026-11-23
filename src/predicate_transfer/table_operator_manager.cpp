#include "predicate_transfer/table_operator_manager.hpp"
#include "predicate_transfer/operator/logical_create_bf.hpp"
#include "predicate_transfer/operator/logical_use_bf.hpp"


namespace duckdb {

bool TableOperatorManager::CheckLogicalCreateBF(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		return op.Cast<const LogicalExtensionOperator>().GetExtensionName() == LogicalCreateBF::kExtensionName;
	}
	return false;
}

bool TableOperatorManager::CheckLogicalUseBF(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR) {
		return op.Cast<const LogicalExtensionOperator>().GetExtensionName() == LogicalUseBF::kExtensionName;
	}
	return false;
}

} // namespace duckdb
