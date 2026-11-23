#pragma once

#include "duckdb.hpp"
#include "predicate_transfer/predicate_transfer_optimizer.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"

namespace duckdb {

class RptExtension : public OptimizerExtension {
public:
	RptExtension() {
		optimize_function = RptOptimizeFunction;
		pre_optimize_function = RptPreOptimizeFunction;
	}

	static void RptPreOptimizeFunction(OptimizerExtensionInput &input, duckdb::unique_ptr<LogicalOperator> &plan);

	static void RptOptimizeFunction(OptimizerExtensionInput &input, duckdb::unique_ptr<LogicalOperator> &plan);

	std::string Name() const;

	std::string Version() const;

	void Load(ExtensionLoader &loader);
};

} // namespace duckdb
