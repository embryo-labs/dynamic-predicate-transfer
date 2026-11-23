#include "duckdb.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/column_index.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/function/scalar_function.hpp"

#include "filter.hpp"

namespace duckdb {

class RPTTableFilter {
 public:
	RPTTableFilter(shared_ptr<RPTFilter> filter, LogicalType input_type)
	    : filter_(std::move(filter)), input_type_(std::move(input_type)) {
	}

	
	struct FilterState : public FunctionLocalState {
		SelectionVector sel;
	};
	
	struct FilterBindData : public FunctionData {
		shared_ptr<RPTFilter> filter_;
		LogicalType input_type_;

		FilterBindData(shared_ptr<RPTFilter> filter, LogicalType input_type) : filter_(filter), input_type_(input_type) {}

		unique_ptr<FunctionData> Copy() const override {
			return make_uniq<FilterBindData>(filter_, input_type_);
		}

		// why??
		// used in Copy() but why??
		bool Equals(const FunctionData &other_p) const override {
			auto &other = other_p.Cast<FilterBindData>();
			return filter_.get() == other.filter_.get();
		}
	};

	unique_ptr<Expression> GetExpression(unique_ptr<Expression> input) const;
 	
 private:
	shared_ptr<RPTFilter> filter_;	
	LogicalType input_type_;

};

}