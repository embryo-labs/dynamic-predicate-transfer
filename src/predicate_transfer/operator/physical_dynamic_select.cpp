#include "predicate_transfer/operator/physical_dynamic_select.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {
PhysicalDynamicSelect::PhysicalDynamicSelect(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                             idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality) {
}

class DynamicSelectState : public OperatorState {
public:
	explicit DynamicSelectState() {
	}

public:
	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op);
	}
};

unique_ptr<OperatorState> PhysicalDynamicSelect::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<DynamicSelectState>();
}

InsertionOrderPreservingMap<string> PhysicalDynamicSelect::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["Name"] = "PhysicalRPTDynamicSelect";
	return result;
}

void PhysicalDynamicSelect::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();

	auto &state = meta_pipeline.GetState();
	state.AddPipelineOperator(current, *this);
	children[1].get().Cast<PhysicalCreateBF>().BuildPipelinesFromRelated(current, meta_pipeline);
	children[0].get().BuildPipelines(current, meta_pipeline);
}

OperatorResultType PhysicalDynamicSelect::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                          GlobalOperatorState &gstate, OperatorState &state_p) const {
	chunk.Reference(input);
	return OperatorResultType::NEED_MORE_INPUT;
}

} // namespace duckdb
