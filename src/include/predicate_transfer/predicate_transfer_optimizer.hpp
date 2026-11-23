#pragma once

#include "transfer_graph_manager.hpp"
#include "operator/logical_create_bf.hpp"
#include "operator/logical_use_bf.hpp"
#include "config.hpp"
#include "duckdb/optimizer/optimizer.hpp"

namespace duckdb {
using BloomFilters = vector<shared_ptr<RPTBloomFilter>>;

class PredicateTransferOptimizer {
public:
	struct TransferStep {
		LogicalOperator* table = nullptr;
		vector<shared_ptr<FilterPlan>> use_bf;
		vector<shared_ptr<FilterPlan>> create_bf;

		string ToString() const;
	};

	explicit PredicateTransferOptimizer(Optimizer &optimizer, ClientContext &context, RPTOptimizerConfig config)
	    : optimizer(optimizer), graph_manager(context, config), config(config) {
	}

	//! Extract the query join information, note that this function must be called before join order optimization,
	//! because some join conditions are lost during join order optimization.
	unique_ptr<LogicalOperator> PreOptimize(unique_ptr<LogicalOperator> plan);

	//! Create bloom filters and insert them into the query plan, note that this function must be called after join
	//! order optimization, because it cannot handle newly inserted operator correctly.
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> plan);

	string GetDebugInfo() const;
	idx_t GetNewFilterPlanID() {
		return global_filter_plan_id++;
	}
	
	unique_ptr<LogicalCreateBF> BuildCreateBFOperator(const LogicalOperator &node, const vector<shared_ptr<FilterPlan>> &bf_plans) const;
  unique_ptr<LogicalUseBF> BuildUseBFOperator(const LogicalOperator &node, const vector<shared_ptr<FilterPlan>> &bf_plans) const;
	Optimizer& GetOptimizer() { return optimizer; }


private:
	unique_ptr<LogicalOperator> InsertTransferOperators(unique_ptr<LogicalOperator> plan, unordered_map<LogicalOperator *, unique_ptr<LogicalOperator>>& modification);
	vector<TransferStep> GenerateNodeOrderPlan();
	unordered_map<LogicalOperator*, unique_ptr<LogicalOperator>> CreateModificationMap(const vector<TransferStep>& steps);
	
	TransferStep CreateBloomFilterPlanOnTransferStep(const TransferGraphManager::TransferStep &step, unordered_map<idx_t, vector<shared_ptr<FilterPlan>>>& bf_to_use_map);

	void GetAllBFsToUse(idx_t cur_node_id, vector<shared_ptr<FilterPlan>> &bfs_to_use_plan, unordered_map<idx_t, vector<shared_ptr<FilterPlan>>>& bf_to_use_map) const;
	void GetAllBFsToCreate(idx_t cur_node_id, vector<shared_ptr<FilterPlan>> &bfs_to_create_plan, unordered_map<idx_t, vector<shared_ptr<FilterPlan>>>& bf_to_use_map, const vector<shared_ptr<GraphEdge>>& edges);
	unique_ptr<LogicalOperator> BuildOpFromStep(const TransferStep& step) const;
  
	// used after table index are changed (e.g. in late materialization)
	void UpdateBindings(LogicalOperator& op);

	
	//! which column(s) involved in this expression?
	static void GetColumnBindingExpression(Expression &expr, vector<BoundColumnRefExpression *> &expressions);

	static bool CheckIfGeneratedRPT(const LogicalOperator &node);

private:
	TransferGraphManager graph_manager;
	Optimizer &optimizer;
	RPTOptimizerConfig config;
	std::vector<TransferStep> final_transfer_steps;


	idx_t global_filter_plan_id = 1;
};
} // namespace duckdb