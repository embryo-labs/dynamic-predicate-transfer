#pragma once

#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb::predicate_transfer_graph {

// A join filter hashes values. Finding a column somewhere inside an expression
// is insufficient: e.g. CASE, COALESCE and rounded casts change its key.
inline const BoundColumnRefExpression *FindKeyColumn(const Expression &expr) {
	switch (expr.GetExpressionType()) {
	case ExpressionType::BOUND_COLUMN_REF:
		return &expr.Cast<BoundColumnRefExpression>();
	case ExpressionType::OPERATOR_CAST: {
		const auto &cast = expr.Cast<BoundFunctionExpression>();
		// DuckDB marks range-proven narrowing casts with an internal function name.
		// Its bind callback address may differ between the host and a loaded extension.
		if (cast.Function().GetName() == InternalCompressedMaterializationCastFun::Name &&
		    BoundCastExpression::IsDefaultCast(cast) && cast.GetReturnType().IsIntegral() &&
		    BoundCastExpression::Child(cast).GetReturnType().IsIntegral()) {
			return FindKeyColumn(BoundCastExpression::Child(cast));
		}
		const auto &source = BoundCastExpression::Child(cast).GetReturnType();
		const auto &target = cast.GetReturnType();
		if (source.IsIntegral() && target.IsIntegral() && BoundCastExpression::CastIsInvertible(source, target) &&
		    !BoundCastExpression::CastCanThrow(source, target, false)) {
			return FindKeyColumn(BoundCastExpression::Child(cast));
		}
		return nullptr;
	}
	case ExpressionType::CASE_EXPR: {
		const auto &case_expr = expr.Cast<BoundCaseExpression>();
		const auto &checks = case_expr.CaseChecks();
		if (checks.size() == 1 && checks[0].then_expr->GetExpressionType() == ExpressionType::BOUND_FUNCTION &&
		    checks[0].then_expr->Cast<BoundFunctionExpression>().Function().GetName() == "error") {
			return FindKeyColumn(case_expr.Else());
		}
		return nullptr;
	}
	case ExpressionType::BOUND_FUNCTION: {
		const auto &func = expr.Cast<BoundFunctionExpression>();
		const auto &name = func.Function().GetName().GetIdentifierName();
		if (name.rfind("__internal_compress_", 0) != 0 && name.rfind("__internal_decompress_", 0) != 0) {
			return nullptr;
		}
		const auto &children = func.GetChildren();
		if (children.empty()) {
			return nullptr;
		}
		for (idx_t i = 1; i < children.size(); i++) {
			if (children[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
				return nullptr;
			}
		}
		return FindKeyColumn(*children.front());
	}
	default:
		return nullptr;
	}
}

} // namespace duckdb::predicate_transfer_graph
