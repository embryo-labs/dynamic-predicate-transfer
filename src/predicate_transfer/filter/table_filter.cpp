#include "predicate_transfer/filter/table_filter.hpp"

#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

namespace duckdb {

unique_ptr<Expression> RPTTableFilter::GetExpression(unique_ptr<Expression> input) const {
	static const vector<idx_t> key_column {0};
	auto eval_func = [](DataChunk &args, ExpressionState &state, Vector &result) {
		auto &filter = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<FilterBindData>().filter_;
		size_t matches = 0;
		filter->Lookup(args, key_column, result, matches);
	};

	ScalarFunction rpt_table_func("rpt_table_filter", {input_type_}, LogicalType::BOOLEAN, eval_func);
	auto init = [](ExpressionState &, const BoundFunctionExpression &, FunctionData *) -> unique_ptr<FunctionLocalState> {
		return make_uniq<FilterState>();
	};
	auto select = [](DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
	                 optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
		auto &filter = state.expr.Cast<BoundFunctionExpression>().BindInfo()->Cast<FilterBindData>().filter_;
		auto &scratch = ExecuteFunctionState::GetFunctionState(state)->Cast<FilterState>().sel;
		auto &local = GetFilterResultSelection(args.size(), sel, false_sel && sel ? nullptr : true_sel, scratch);
		size_t matches = 0;
		filter->Lookup(args, key_column, local, matches);
		// When both outputs are requested the input selection can alias either one.
		// Read each source index before compacting it into the matching output.
		if (sel && true_sel && false_sel) {
			idx_t pass = 0, fail = 0;
			for (idx_t i = 0; i < args.size(); ++i) {
				auto row = sel->get_index(i);
				if (pass < matches && local.get_index(pass) == i) {
					true_sel->set_index(pass++, row);
				} else {
					false_sel->set_index(fail++, row);
				}
			}
			return matches;
		}
		return TranslateSelection(args.size(), sel, local, matches, true_sel, false_sel);
	};
	rpt_table_func.SetSelectCallback(select);
	auto group_prune = [](const FunctionStatisticsPruneInput &input) {
		auto &bind_data = input.bind_data->Cast<FilterBindData>();
		auto stats = input.ChildStats(0);
		return stats ? bind_data.filter_->CheckStatistics(*stats) : FilterPropagateResult::NO_PRUNING_POSSIBLE;
	};

	rpt_table_func.SetInitStateCallback(init);
	rpt_table_func.SetFilterPruneCallback(group_prune);

	// Create BoundFunctionExpression
	auto bind_data = make_uniq<FilterBindData>(filter_, input_type_);
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(input));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(rpt_table_func), std::move(children),
	                                          std::move(bind_data));
}

}
