#include "predicate_transfer/dag.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

bool FilterPlan::operator==(const FilterPlan &other) const {
	return build == other.build && apply == other.apply && return_types == other.return_types &&
	       exact_row_ids == other.exact_row_ids;
}

GraphEdge *GraphNode::Add(idx_t other, bool is_forward, bool is_in_edge) {
	auto &stage = (is_forward ? forward_stage_edges : backward_stage_edges);
	auto &edges = (is_in_edge ? stage.in : stage.out);
	for (auto &edge : edges) {
		if (edge->destination == other) {
			return edge.get();
		}
	}
	edges.emplace_back(make_shared_ptr<GraphEdge>(other));
	return edges.back().get();
}

GraphEdge *GraphNode::Add(idx_t other, const ColumnBinding &left_cols, const ColumnBinding &right_cols,
                          const LogicalType &types, bool is_forward, bool is_in_edge) {
	auto *edge = Add(other, is_forward, is_in_edge);
	edge->left.push_back(left_cols);
	edge->right.push_back(right_cols);
	edge->return_types.push_back(types);
	return edge;
}

GraphEdge *GraphNode::Add(idx_t other, const shared_ptr<FilterPlan> &filter_plan, bool is_forward, bool is_in_edge) {
	auto *edge = Add(other, is_forward, is_in_edge);
	edge->filter_plan.push_back(filter_plan);
	return edge;
}

void FilterPlan::Serialize(Serializer &serializer) const {
	serializer.WritePropertyWithDefault<vector<ColumnBinding>>(200, "build", build);
	serializer.WritePropertyWithDefault<vector<ColumnBinding>>(201, "apply", apply);
	serializer.WritePropertyWithDefault<vector<LogicalType>>(202, "return_types", return_types);
	serializer.WritePropertyWithDefault<idx_t>(205, "plan_id", plan_id);
	serializer.WritePropertyWithDefault<bool>(206, "exact_row_ids", exact_row_ids);
}

unique_ptr<FilterPlan> FilterPlan::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<FilterPlan>(new FilterPlan());
	deserializer.ReadPropertyWithDefault<vector<ColumnBinding>>(200, "build", result->build);
	deserializer.ReadPropertyWithDefault<vector<ColumnBinding>>(201, "apply", result->apply);
	deserializer.ReadPropertyWithDefault<vector<LogicalType>>(202, "return_types", result->return_types);
	deserializer.ReadPropertyWithDefault<idx_t>(205, "plan_id", result->plan_id);
	deserializer.ReadPropertyWithDefault<bool>(206, "exact_row_ids", result->exact_row_ids);
	return result;
}


string FilterPlan::ToString() const {
	stringstream ss;
	ss << "plan_id=" << plan_id << ", ";
	
	auto vec_to_str = [&](vector<ColumnBinding> v) {
		for (auto& x : v) {
			ss << x.ToString() << ", ";
		}
	};
	auto vectype_to_str = [&](vector<LogicalType> v) {
		for (auto& x : v) {
			ss << x.ToString() << ", ";
		}
	};

	auto vecint_to_str = [&](vector<idx_t> v) {
		for (auto& x : v) {
			ss << x << ", ";
		}
	};
	ss << "build=(";
	vec_to_str(build);
	ss << "), ";

	ss << "apply=(";
	vec_to_str(apply);
	ss << "), ";

	ss << "return_types=(";
	vectype_to_str(return_types);
	ss << ")";

	return ss.str();
}

} // namespace duckdb
