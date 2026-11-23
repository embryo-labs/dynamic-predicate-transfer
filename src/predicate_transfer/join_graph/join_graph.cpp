#include "predicate_transfer/join_graph/join_graph.hpp"

#include <algorithm>

#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"

#include <queue>

namespace duckdb::predicate_transfer_graph {

//===--------------------------------------------------------------------===//
// JoinKeyTableGroup
//===--------------------------------------------------------------------===//

void JoinKeyTableGroup::Union(const JoinKeyTableGroup &other) {
	table_ids.insert(other.table_ids.begin(), other.table_ids.end());
}

string JoinKeyTableGroup::ToString() const {
	stringstream ss;
	ss << "(ids=[";
	for (auto &id : table_ids) {
		ss << "(" << id.first << ", " << id.second.ToString() << "), ";
	}
	ss << "])";
	return ss.str();
}

//===--------------------------------------------------------------------===//
// EdgeInfo
//===--------------------------------------------------------------------===//

string EdgeInfo::ToString() const {
	stringstream ss;
	ss << "(left_table=";
	ss << TableOperatorManager::GetScalarTableIndex(left_table);
	ss << ", right_table=";
	ss << TableOperatorManager::GetScalarTableIndex(right_table);
	ss << ", keys=[";
	for (idx_t i = 0; i < left_bindings.size(); ++i) {
		if (i > 0) {
			ss << ", ";
		}
		ss << left_bindings[i].ToString() << "=" << right_bindings[i].ToString() << ":" << return_types[i].ToString();
	}
	ss << "]";
	ss << ", protect_left=" << protect_left;
	ss << ", protect_right=" << protect_right;
	ss << ")";
	return ss.str();
}

//===--------------------------------------------------------------------===//
// JoinGraph — Union-Find helpers
//===--------------------------------------------------------------------===//

using BindingParentMap = unordered_map<ColumnBinding, ColumnBinding, ColumnBindingHashFunc>;

static ColumnBinding FindBindingRoot(const ColumnBinding &binding, BindingParentMap &parents) {
	auto it = parents.find(binding);
	if (it == parents.end()) {
		return binding;
	}
	ColumnBinding root = it->second;
	if (root != binding) {
		root = FindBindingRoot(root, parents);
		parents[binding] = root; // Path compression
	}
	return root;
}

static void UnionBindings(const ColumnBinding &a, const ColumnBinding &b, const LogicalType &type,
                          BindingParentMap &parents, BindingGroupMap &group_map) {
	ColumnBinding root_a = FindBindingRoot(a, parents);
	ColumnBinding root_b = FindBindingRoot(b, parents);
	if (root_a == root_b) {
		return;
	}

	// Union by attaching b to a
	parents[root_b] = root_a;

	auto &group_a = group_map[root_a];
	if (!group_a) {
		group_a = make_shared_ptr<JoinKeyTableGroup>(a.table_index.index, a, type);
	}

	auto &group_b = group_map[root_b];
	if (!group_b) {
		group_b = make_shared_ptr<JoinKeyTableGroup>(b.table_index.index, b, type);
	}

	group_a->Union(*group_b);
	group_map[root_a] = group_a;
}

//===--------------------------------------------------------------------===//
// JoinGraph — AddEdgePair
//===--------------------------------------------------------------------===//

void JoinGraph::AddEdgePair(LogicalOperator &left_node, vector<ColumnBinding> left_bindings,
                                   LogicalOperator &right_node, vector<ColumnBinding> right_bindings,
                                   vector<LogicalType> forward_types, bool protect_left, bool protect_right) {
	idx_t left_id = TableOperatorManager::GetScalarTableIndex(left_node);
	idx_t right_id = TableOperatorManager::GetScalarTableIndex(right_node);

	EdgeInfo edge(std::move(forward_types), left_node, std::move(left_bindings), right_node, std::move(right_bindings));
	edge.protect_left = protect_left;
	edge.protect_right = protect_right;

	// The flipped twin keeps the same return_types as the forward edge.
	// Both sides of an equality join share the same logical type, and the
	// authoritative type comes from the JoinKeyTableGroup (or the original
	// join condition).  Do NOT re-resolve via right_node.types[col] — post-
	// rename column_index is not guaranteed to index into the operator's
	// output `types` once a compression projection has shrunk it.
	EdgeInfo flipped = edge.Flip();

	neighbor_matrix[left_id][right_id].push_back(std::move(edge));
	neighbor_matrix[right_id][left_id].push_back(std::move(flipped));
}

//===--------------------------------------------------------------------===//
// JoinGraph — Build
//===--------------------------------------------------------------------===//

bool JoinGraph::Build(LogicalOperator &plan, bool bundle_composite_edges) {
	neighbor_matrix.clear();
	table_groups.clear();
	table_operator_manager.Build(plan);
	if (table_operator_manager.GetAllTableOperators().size() < 2) {
		return false;
	}
	ExtractEdgesInfo(table_operator_manager.GetAllJoinOperators());
	DiscoverEdges(bundle_composite_edges);
	return !neighbor_matrix.empty();
}

//===--------------------------------------------------------------------===//
// JoinGraph — ExtractEdgesInfo helpers
//===--------------------------------------------------------------------===//

// Orientation-insensitive fingerprint of a join edge. All key_cols entries
// share the same (lo_table, hi_table), so only column_index values vary.
struct EdgeKey {
	idx_t lo_table;
	idx_t hi_table;
	vector<pair<idx_t, idx_t>> key_cols; // (lo_side_col, hi_side_col) per join key

	bool operator==(const EdgeKey &other) const {
		return lo_table == other.lo_table && hi_table == other.hi_table && key_cols == other.key_cols;
	}

	struct Hash {
		size_t operator()(const EdgeKey &k) const {
			size_t h = std::hash<idx_t> {}(k.lo_table) ^ (std::hash<idx_t> {}(k.hi_table) << 1U);
			for (auto &p : k.key_cols) {
				h ^= (std::hash<idx_t> {}(p.first) << 1U) + std::hash<idx_t> {}(p.second);
			}
			return h;
		}
	};
};

// Build an orientation-insensitive fingerprint for one (left_table, right_table)
// group of key columns. Sorted so differently-ordered conditions collapse.
static EdgeKey MakeEdgeKey(idx_t left_table, idx_t right_table, const vector<ColumnBinding> &left_bindings,
                           const vector<ColumnBinding> &right_bindings) {
	EdgeKey key;
	const bool forward = left_table <= right_table;
	key.lo_table = forward ? left_table : right_table;
	key.hi_table = forward ? right_table : left_table;
	for (idx_t i = 0; i < left_bindings.size(); ++i) {
		idx_t lo_col = (forward ? left_bindings[i] : right_bindings[i]).column_index;
		idx_t hi_col = (forward ? right_bindings[i] : left_bindings[i]).column_index;
		key.key_cols.emplace_back(lo_col, hi_col);
	}
	std::sort(key.key_cols.begin(), key.key_cols.end());
	return key;
}

// Decide whether this join participates in PT, and which side (if any) must
// be protected from filter injection. Returns false if the join type is not
// safe to use for predicate transfer.
static bool ComputeJoinProtection(const LogicalComparisonJoin &join, bool &protect_left, bool &protect_right) {
	protect_left = false;
	protect_right = false;
	switch (join.type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		switch (join.join_type) {
		case JoinType::LEFT:
			protect_left = true;
			return true;
		case JoinType::RIGHT:
		case JoinType::MARK:
			protect_right = true;
			return true;
		case JoinType::INNER:
		case JoinType::SEMI:
		case JoinType::RIGHT_SEMI:
			return true;
		default:
			return false;
		}
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
		if (join.delim_flipped == 0) {
			protect_left = true;
		} else {
			protect_right = true;
		}
		return true;
	default:
		return false;
	}
}

// Default to the join condition's own type. If rename resolved to a
// different binding AND the scan's output type for that column differs
// (a compression projection inserted a type cast, e.g. BIGINT → UINTEGER),
// use the scan's type because the BF operates on the uncompressed column.
static LogicalType ResolveSourceKeyType(const ColumnBinding &pre_rename, const ColumnBinding &post_rename,
                                        LogicalOperator &node, const LogicalType &fallback) {
	if (pre_rename == post_rename) {
		return fallback;
	}

	// A binding's column index is stable across projection pruning and may
	// not equal its position in the operator's output types.
	const auto bindings = node.GetColumnBindings();
	const auto it = std::find(bindings.begin(), bindings.end(), post_rename);
	if (it != bindings.end()) {
		const auto output_index = static_cast<idx_t>(it - bindings.begin());
		if (output_index < node.types.size()) {
			return node.types[output_index];
		}
	}
	return fallback;
}

//===--------------------------------------------------------------------===//
// JoinGraph — ExtractEdgesInfo
//===--------------------------------------------------------------------===//

void JoinGraph::ExtractEdgesInfo(const vector<reference<LogicalOperator>> &join_operators) {
	// Parallel vectors of conditions for one (left_table, right_table) pair.
	struct Conditions {
		vector<ColumnBinding> left_bindings;
		vector<ColumnBinding> right_bindings;
		vector<LogicalType> types;
	};

	unordered_set<EdgeKey, EdgeKey::Hash> existed_edges;
	BindingParentMap binding_parents;
	BindingGroupMap group_map;

	for (auto &join_ref : join_operators) {
		auto &join = join_ref.get();
		if (join.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
		    join.type != LogicalOperatorType::LOGICAL_DELIM_JOIN) {
			continue;
		}
		auto &comp_join = join.Cast<LogicalComparisonJoin>();
		D_ASSERT(comp_join.expressions.empty());

		bool protect_left, protect_right;
		if (!ComputeJoinProtection(comp_join, protect_left, protect_right)) {
			continue;
		}

		// Group conditions by resolved (left_table, right_table). Nesting handles
		// the rare case of a comparison join whose subtrees each contain multiple
		// base tables and whose conditions target different pairs.
		unordered_map<idx_t, unordered_map<idx_t, Conditions>> by_table_pair;

		for (auto &cond : comp_join.conditions) {
			if (!cond.IsComparison() || cond.GetComparisonType() != ExpressionType::COMPARE_EQUAL ||
			    cond.GetLHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF ||
			    cond.GetRHS().GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
				continue;
			}

			auto &left_expr = cond.GetLHS().Cast<BoundColumnRefExpression>();
			auto &right_expr = cond.GetRHS().Cast<BoundColumnRefExpression>();
			ColumnBinding left_binding = table_operator_manager.GetRenaming(left_expr.Binding());
			ColumnBinding right_binding = table_operator_manager.GetRenaming(right_expr.Binding());

			auto *left_node = table_operator_manager.GetTableOperator(left_binding.table_index.index);
			auto *right_node = table_operator_manager.GetTableOperator(right_binding.table_index.index);
			if (!left_node || !right_node) {
				continue;
			}

			auto left_key_type =
			    ResolveSourceKeyType(left_expr.Binding(), left_binding, *left_node, cond.GetLHS().GetReturnType());
			auto right_key_type =
			    ResolveSourceKeyType(right_expr.Binding(), right_binding, *right_node, cond.GetRHS().GetReturnType());
			// A per-column table filter hashes the scan column in its storage
			// type. If the two resolved inputs differ (e.g. TINYINT projected
			// as BIGINT and joined to a BIGINT column), one Bloom filter cannot
			// represent both sides without an explicit cast expression. Leave
			// this edge to DuckDB's native cast-aware join filter.
			if (left_key_type != right_key_type) {
				continue;
			}

			auto &bucket = by_table_pair[left_binding.table_index.index][right_binding.table_index.index];
			bucket.left_bindings.push_back(left_binding);
			bucket.right_bindings.push_back(right_binding);
			bucket.types.push_back(left_key_type);

			if (!protect_left && !protect_right) {
				UnionBindings(left_binding, right_binding, left_key_type, binding_parents, group_map);
			}
		}

		// Emit one EdgeInfo per (left_table, right_table) group.
		for (auto &lhs_entry : by_table_pair) {
			for (auto &rhs_entry : lhs_entry.second) {
				auto &bucket = rhs_entry.second;
				if (bucket.left_bindings.empty()) {
					continue;
				}

				auto key = MakeEdgeKey(lhs_entry.first, rhs_entry.first, bucket.left_bindings, bucket.right_bindings);
				if (!existed_edges.insert(std::move(key)).second) {
					continue;
				}

				auto *left_node = table_operator_manager.GetTableOperator(lhs_entry.first);
				auto *right_node = table_operator_manager.GetTableOperator(rhs_entry.first);
				if (!left_node || !right_node) {
					continue;
				}

				AddEdgePair(*left_node, std::move(bucket.left_bindings), *right_node, std::move(bucket.right_bindings),
				            std::move(bucket.types), protect_left, protect_right);
			}
		}
	}

	// Finalize table_groups by resolving root bindings.
	for (auto &entry : group_map) {
		ColumnBinding rep = FindBindingRoot(entry.first, binding_parents);
		table_groups[entry.first] = group_map[rep];
	}
}

//===--------------------------------------------------------------------===//
// JoinGraph — DiscoverEdges
//===--------------------------------------------------------------------===//

// Canonicalized "(table_a.col_a == table_b.col_b) is already represented by
// some edge" key: orientation-insensitive so a single lookup handles both
// directions.
struct CoveredColumnPair {
	idx_t lo_table, hi_table;
	ColumnBinding lo_col, hi_col;
	bool operator==(const CoveredColumnPair &o) const {
		return lo_table == o.lo_table && hi_table == o.hi_table && lo_col == o.lo_col && hi_col == o.hi_col;
	}
};
struct CoveredColumnPairHash {
	size_t operator()(const CoveredColumnPair &p) const {
		ColumnBindingHashFunc ch;
		return std::hash<idx_t> {}(p.lo_table) ^ (std::hash<idx_t> {}(p.hi_table) << 1U) ^ (ch(p.lo_col) << 2U) ^
		       (ch(p.hi_col) << 3U);
	}
};

static CoveredColumnPair MakeCoveredKey(idx_t t1, const ColumnBinding &c1, idx_t t2, const ColumnBinding &c2) {
	if (t1 <= t2) {
		return CoveredColumnPair {t1, t2, c1, c2};
	}
	return CoveredColumnPair {t2, t1, c2, c1};
}

void JoinGraph::DiscoverEdges(bool bundle_composite_edges) {
	// Add transitively-derived edges from the join-key equivalence classes.
	// If the query says A.x = B.x AND B.x = C.x, the user never wrote A.x = C.x
	// but the equality is implied — and the missing A↔C edge is exactly the
	// kind of short-circuit path predicate transfer wants, since flooding
	// propagates one edge per round.
	//
	// Judgement is per (column, column) pair, NOT per (table, table) pair:
	// two tables that are already connected via (a1, b1) still need a new
	// edge (a2, b2) if another equivalence class links a second pair of their
	// columns.

	// Seed `covered` with every column-pair already carried by an existing
	// edge. AddEdgePair keeps neighbor_matrix symmetric, so we only walk one
	// direction (lo_table → hi_table) to avoid double-counting. Subsequently,
	// `covered.insert(key).second` serves as both the "is this new?" check
	// and the "mark it present" step, so closure edges added in this pass
	// also prevent duplicate insertions from later equivalence classes.
	unordered_set<CoveredColumnPair, CoveredColumnPairHash> covered;
	for (auto &outer : neighbor_matrix) {
		idx_t t1 = outer.first;
		for (auto &inner : outer.second) {
			idx_t t2 = inner.first;
			if (t1 > t2) {
				continue;
			}
			for (auto &edge : inner.second) {
				for (idx_t k = 0; k < edge.left_bindings.size(); ++k) {
					covered.insert(MakeCoveredKey(t1, edge.left_bindings[k], t2, edge.right_bindings[k]));
				}
			}
		}
	}

	// When bundle_composite_edges is enabled, accumulate transitive column
	// pairs by (lo_table, hi_table) so that multiple equivalence classes
	// linking the SAME pair collapse into one composite edge.
	// When disabled, emit each pair as an independent single-key edge.
	struct PendingEdge {
		vector<ColumnBinding> lo_bindings;
		vector<ColumnBinding> hi_bindings;
		vector<LogicalType> types;
	};
	unordered_map<idx_t, unordered_map<idx_t, PendingEdge>> pending;

	// Iterate each JoinKeyTableGroup exactly once. table_groups has one entry
	// per binding, and multiple bindings in the same equivalence class share
	// one shared_ptr, so we dedup by pointer identity.
	unordered_set<JoinKeyTableGroup *> seen_groups;
	for (auto &entry : table_groups) {
		auto *group = entry.second.get();
		if (!group || !seen_groups.insert(group).second) {
			continue;
		}
		if (group->table_ids.size() < 2) {
			continue;
		}

		vector<pair<idx_t, ColumnBinding>> members(group->table_ids.begin(), group->table_ids.end());
		for (idx_t i = 0; i < members.size(); ++i) {
			for (idx_t j = i + 1; j < members.size(); ++j) {
				idx_t t1 = members[i].first;
				idx_t t2 = members[j].first;
				if (t1 == t2) {
					continue;
				}
				ColumnBinding c1 = members[i].second;
				ColumnBinding c2 = members[j].second;

				if (!covered.insert(MakeCoveredKey(t1, c1, t2, c2)).second) {
					continue; // Already represented by some edge.
				}

				if (bundle_composite_edges) {
					// Normalize to (lo, hi) so pairs across different
					// equivalence classes accumulate into one bucket.
					idx_t lo = t1, hi = t2;
					ColumnBinding lo_col = c1, hi_col = c2;
					if (lo > hi) {
						std::swap(lo, hi);
						std::swap(lo_col, hi_col);
					}
					auto &pe = pending[lo][hi];
					pe.lo_bindings.push_back(lo_col);
					pe.hi_bindings.push_back(hi_col);
					pe.types.push_back(group->return_type);
				} else {
					auto *table_a = table_operator_manager.GetTableOperator(t1);
					auto *table_b = table_operator_manager.GetTableOperator(t2);
					D_ASSERT(table_a && table_b);
					AddEdgePair(*table_a, {c1}, *table_b, {c2}, {group->return_type},
					            /*protect_left=*/false, /*protect_right=*/false);
				}
			}
		}
	}

	for (auto &outer : pending) {
		idx_t lo = outer.first;
		for (auto &inner : outer.second) {
			idx_t hi = inner.first;
			auto *table_lo = table_operator_manager.GetTableOperator(lo);
			auto *table_hi = table_operator_manager.GetTableOperator(hi);
			D_ASSERT(table_lo && table_hi);
			AddEdgePair(*table_lo, std::move(inner.second.lo_bindings), *table_hi, std::move(inner.second.hi_bindings),
			            std::move(inner.second.types),
			            /*protect_left=*/false, /*protect_right=*/false);
		}
	}

	if (bundle_composite_edges) {
		BundleCompositeEdges();
	}
}

void JoinGraph::BundleCompositeEdges() {
	struct EdgeBundle {
		bool protect_left;
		bool protect_right;
		vector<ColumnBinding> left_bindings;
		vector<ColumnBinding> right_bindings;
		vector<LogicalType> return_types;
	};

	for (auto &outer : neighbor_matrix) {
		idx_t left_id = outer.first;
		for (auto &inner : outer.second) {
			idx_t right_id = inner.first;
			if (left_id >= right_id || inner.second.size() < 2) {
				continue;
			}

			vector<EdgeBundle> bundles;
			for (auto &edge : inner.second) {
				auto bundle_it = std::find_if(bundles.begin(), bundles.end(), [&](const EdgeBundle &bundle) {
					return bundle.protect_left == edge.protect_left && bundle.protect_right == edge.protect_right;
				});
				if (bundle_it == bundles.end()) {
					bundles.push_back({edge.protect_left, edge.protect_right, {}, {}, {}});
					bundle_it = bundles.end() - 1;
				}

				for (idx_t key_idx = 0; key_idx < edge.KeyCount(); key_idx++) {
					bool duplicate = false;
					for (idx_t existing_idx = 0; existing_idx < bundle_it->left_bindings.size(); existing_idx++) {
						if (bundle_it->left_bindings[existing_idx] == edge.left_bindings[key_idx] &&
						    bundle_it->right_bindings[existing_idx] == edge.right_bindings[key_idx]) {
							duplicate = true;
							break;
						}
					}
					if (duplicate) {
						continue;
					}
					bundle_it->left_bindings.push_back(edge.left_bindings[key_idx]);
					bundle_it->right_bindings.push_back(edge.right_bindings[key_idx]);
					bundle_it->return_types.push_back(edge.return_types[key_idx]);
				}
			}

			auto *left_table = table_operator_manager.GetTableOperator(left_id);
			auto *right_table = table_operator_manager.GetTableOperator(right_id);
			D_ASSERT(left_table && right_table);

			vector<EdgeInfo> merged;
			merged.reserve(bundles.size());
			for (auto &bundle : bundles) {
				EdgeInfo edge(std::move(bundle.return_types), *left_table, std::move(bundle.left_bindings),
				              *right_table, std::move(bundle.right_bindings));
				edge.protect_left = bundle.protect_left;
				edge.protect_right = bundle.protect_right;
				merged.push_back(std::move(edge));
			}
			inner.second = std::move(merged);

			auto reverse_outer = neighbor_matrix.find(right_id);
			D_ASSERT(reverse_outer != neighbor_matrix.end());
			auto reverse_inner = reverse_outer->second.find(left_id);
			D_ASSERT(reverse_inner != reverse_outer->second.end());
			reverse_inner->second.clear();
			reverse_inner->second.reserve(inner.second.size());
			for (auto &edge : inner.second) {
				reverse_inner->second.push_back(edge.Flip());
			}
		}
	}
}

//===--------------------------------------------------------------------===//
// JoinGraph — Debug output
//===--------------------------------------------------------------------===//

string JoinGraph::EdgesToString() const {
	stringstream ss;
	ss << "all_edges=(";
	for (const auto &a : neighbor_matrix) {
		ss << "\n\t From " << a.first << ": ";
		for (const auto &b : a.second) {
			ss << "\n\t\t To " << b.first << " ->";
			for (const auto &edge : b.second) {
				ss << edge.ToString() << ", ";
			}
		}
	}
	ss << ")\n";
	return ss.str();
}

} // namespace duckdb::predicate_transfer_graph
