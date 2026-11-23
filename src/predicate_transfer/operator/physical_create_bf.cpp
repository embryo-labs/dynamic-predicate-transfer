#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include <iostream>
#include <mutex>
#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"
#include "predicate_transfer/operator/physical_use_bf.hpp"
#include "predicate_transfer/operator/physical_create_bf.hpp"
#include "predicate_transfer/filter/bitmap_filter.hpp"
#include "predicate_transfer/filter/table_filter.hpp"

#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/execution/operator/aggregate/ungrouped_aggregate_state.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/filter/constant_filter.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/optimizer/filter_combiner.hpp"
#include "duckdb/planner/filter/in_filter.hpp"
#include "duckdb/planner/filter/optional_filter.hpp"

namespace duckdb {

PhysicalCreateBF::PhysicalCreateBF(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                   const vector<FilterPhysicalPlan> &filter_plans,
                                   vector<shared_ptr<DynamicTableFilterSet>> dynamic_filter_sets,
                                   vector<vector<ColumnBinding>> &dynamic_filter_cols,
                                   const vector<vector<JoinFilterPushdownColumn>> &scan_filter_columns,
                                   idx_t estimated_cardinality,
                                   bool is_probing_side, const Config &config)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTENSION, std::move(types), estimated_cardinality),
      is_probing_side(is_probing_side), filter_plans(filter_plans), unique_rpt_filters(),
      min_max_applied_cols(std::move(dynamic_filter_cols)), scan_filter_columns(scan_filter_columns),
      min_max_to_create(std::move(dynamic_filter_sets)),
      config(config) {
	for (size_t i = 0; i < filter_plans.size(); ++i) {
		auto &plan = filter_plans[i];
		auto &cols_build = plan.bound_cols;

		if (unique_rpt_filters.find(cols_build) == unique_rpt_filters.end()) {
			unique_rpt_filters[cols_build] = make_shared_ptr<RPTFilterWrapper>();
		}
		auto bf = unique_rpt_filters[cols_build];
		bf_to_create.emplace_back(make_shared_ptr<RPTFilterUsage>(bf));
	}
}

// Keep the original cast responsible for any conversion error. The scan
// filter evaluates the reconstructed cast as TRY_CAST and retains failures.
static unique_ptr<Expression> PreserveScanCastErrors(unique_ptr<Expression> filter,
                                                     unique_ptr<Expression> null_check_input) {
	if (!null_check_input) {
		return filter;
	}
	auto is_null =
	    ExpressionFilter::CreateNullCheckExpression(std::move(null_check_input), ExpressionType::OPERATOR_IS_NULL);
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(is_null), std::move(filter));
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
class CreateBFGlobalSinkState : public GlobalSinkState {
public:
	CreateBFGlobalSinkState(ClientContext &context, const PhysicalCreateBF &op)
	    : context(context), op(op),
	      num_threads(NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads())),
	      temporary_memory_state(TemporaryMemoryManager::Get(context).Register(context)), is_selectivity_checked(false),
	      num_input_rows(0), total_row_size(0) {
		data_collection = make_uniq<ColumnDataCollection>(context, op.types);

		// init min max aggregation
		vector<AggregateFunction> aggr_functions;
		aggr_functions.push_back(MinFunction::GetFunction());
		aggr_functions.push_back(MaxFunction::GetFunction());

		for (idx_t i = 0; i < op.filter_plans.size(); ++i) {
			if (op.min_max_to_create[i] == nullptr) {
				continue;
			}

			auto &plan = op.filter_plans[i];
			for (idx_t j = 0; j < plan.bound_cols.size(); ++j) {
				auto &expr = plan.logical_plan->build[j];
				auto &bound_col_idx = plan.bound_cols[j];
				auto &type = plan.logical_plan->return_types[j];

				if (col_to_expr.count(bound_col_idx)) {
					continue;
				}
				col_to_expr[bound_col_idx] = min_max_aggregates.size();

				for (auto &aggr : aggr_functions) {
					FunctionBinder function_binder(context);
					vector<unique_ptr<Expression>> aggr_children;
					auto bound_col_expr = make_uniq<BoundColumnRefExpression>(type, expr);
					aggr_children.push_back(std::move(bound_col_expr));
					auto aggr_expr = function_binder.BindAggregateFunction(aggr, std::move(aggr_children), nullptr,
					                                                       AggregateType::NON_DISTINCT);
					min_max_aggregates.push_back(std::move(aggr_expr));
				}
			}
		}

		global_aggregate_state =
		    make_uniq<GlobalUngroupedAggregateState>(BufferAllocator::Get(context), min_max_aggregates);
	}

	void FinalizeMinMax() {
		vector<LogicalType> min_max_types;
		for (auto &aggr_expr : min_max_aggregates) {
			min_max_types.push_back(aggr_expr->GetReturnType());
		}
		auto final_min_max = make_uniq<DataChunk>();
		final_min_max->Initialize(Allocator::DefaultAllocator(), min_max_types);
		global_aggregate_state->Finalize(*final_min_max);

		// create a filter for each of the aggregates
		for (idx_t idx = 0; idx < op.filter_plans.size(); idx++) {
			auto &filter_plan = op.filter_plans[idx];
			auto &dynamic_filters = op.min_max_to_create[idx];
			auto &applied_bindings = op.min_max_applied_cols[idx];
			if (dynamic_filters == nullptr) {
				continue;
			}

			for (idx_t expr_idx = 0; expr_idx < filter_plan.bound_cols.size(); expr_idx++) {
				auto apply_col_index = applied_bindings[expr_idx].column_index;
				idx_t build_col_index = filter_plan.bound_cols[expr_idx];
				idx_t agg_idx = col_to_expr[build_col_index];

				auto min_val = final_min_max->data[agg_idx].GetValue(0);
				auto max_val = final_min_max->data[agg_idx + 1].GetValue(0);

				if (min_val.IsNull() || max_val.IsNull()) {
					// it means that no rows can pass the min-max filter...
					min_val = Value::MaximumValue(min_val.type());
					max_val = Value::MinimumValue(max_val.type());
				}

				if (min_val.type().IsIntegral() && max_val.type() == min_val.type() &&
				    min_val.type() != LogicalType::HUGEINT && min_val.type() != LogicalType::UHUGEINT) {
				auto lower = min_val.TryCastAs(context, LogicalType::BIGINT);
				auto upper = max_val.TryCastAs(context, LogicalType::BIGINT);
				if (lower && upper) {
					val_ranges[build_col_index] = make_pair(BigIntValue::Get(*lower), BigIntValue::Get(*upper));
				}
				}
			}
		}
	}

	void FinalizeOrFilter() {
		// create a filter for each of the aggregates
		idx_t num_rows = data_collection->Count();
		auto dynamic_or_filter_threshold = Settings::Get<DynamicOrFilterThresholdSetting>(context);
		// std::cerr << "FinalizeOrFilter: " << num_rows << ", " << dynamic_or_filter_threshold << std::endl;
		if (num_rows > dynamic_or_filter_threshold) {
			return;
		}
		if (!op.config.use_dynamic_in_filter) {
			return;
		}
		DataChunk chunk;
		data_collection->InitializeScanChunk(chunk);
		const auto chunk_count = data_collection->ChunkCount();
		unordered_map<idx_t, value_set_t> distinct_value_map;
		for (idx_t i = 0; i < chunk_count; i++) {
			data_collection->FetchChunk(i, chunk);
			for (auto &col_pair : col_to_expr) {
				auto col_idx = col_pair.first;
				for (int j = 0; j < chunk.size(); j++) {
					distinct_value_map[col_idx].emplace(chunk.GetValue(col_idx, j));
				}
			}
		}
		for (idx_t idx = 0; idx < op.filter_plans.size(); idx++) {
			auto &filter_plan = op.filter_plans[idx];
			auto &dynamic_filters = op.min_max_to_create[idx];
			auto &applied_bindings = op.min_max_applied_cols[idx];
			if (dynamic_filters == nullptr) {
				continue;
			}

			for (idx_t expr_idx = 0; expr_idx < filter_plan.bound_cols.size(); expr_idx++) {
				auto apply_col_index = applied_bindings[expr_idx].column_index;
				idx_t build_col_index = filter_plan.bound_cols[expr_idx];

				auto &distinct_values = distinct_value_map[build_col_index];
				vector<Value> in_list(distinct_values.begin(), distinct_values.end());

				if (in_list.empty() || FilterCombiner::ContainsNull(in_list) || FilterCombiner::IsDenseRange(in_list)) {
					continue;
				}

				const auto &column = op.scan_filter_columns[idx][expr_idx];
				bool preserves_cast_errors;
				auto subject = RuntimeFilterCastUtil::CreateRuntimeFilterInputExpression(context, column,
				                                                                        preserves_cast_errors);
				auto null_check_input = preserves_cast_errors ? subject->Copy() : nullptr;
				auto in_expr = ExpressionFilter::CreateInExpression(std::move(subject), std::move(in_list));
				in_expr = PreserveScanCastErrors(std::move(in_expr), std::move(null_check_input));
				auto filter = make_uniq<ExpressionFilter>(
				    CreateOptionalFilterExpression(std::move(in_expr), column.storage_type));
				dynamic_filters->PushFilter(op, apply_col_index, std::move(filter));
			}
		}
	}

	void ScheduleFinalize(Pipeline &pipeline, Event &event);

public:
	ClientContext &context;
	const PhysicalCreateBF &op;

	const idx_t num_threads;
	unique_ptr<ColumnDataCollection> data_collection;
	vector<unique_ptr<ColumnDataCollection>> local_data_collections;

	//! Global Min/Max aggregates for filter pushdown
	unordered_map<idx_t, idx_t> col_to_expr;
	unique_ptr<GlobalUngroupedAggregateState> global_aggregate_state;
	vector<unique_ptr<Expression>> min_max_aggregates;

	//! If memory is not enough, give up creating BFs
	unique_ptr<TemporaryMemoryState> temporary_memory_state;

	// runtime statistics
	std::once_flag abandon_init;
	bool can_abandon = false;
	idx_t source_rows = 0;
	atomic<bool> abandoned {false};
	atomic<idx_t> input_chunks {0};
	double observed_selectivity = 0;
	string abandon_reason;
	atomic<bool> is_selectivity_checked;
	atomic<int64_t> num_input_rows;
	atomic<int64_t> total_row_size;
	unordered_map<idx_t, pair<int64_t, int64_t>> val_ranges;
};

class CreateBFLocalSinkState : public LocalSinkState {
public:
	CreateBFLocalSinkState(ClientContext &context, const PhysicalCreateBF &op) : client_context(context) {
		auto &gstate = op.sink_state->Cast<CreateBFGlobalSinkState>();
		// An upstream builder can restore the original scan after this sink's
		// global state was created. Inspect the pipeline when its workers start.
		std::call_once(gstate.abandon_init, [&] { gstate.can_abandon = op.CanAbandon(context, gstate.source_rows); });
		local_data = make_uniq<ColumnDataCollection>(context, op.types);
		col_to_expr = gstate.col_to_expr;
		local_aggregate_state = make_uniq<LocalUngroupedAggregateState>(*gstate.global_aggregate_state);

		// How much is the memory that used to materialize the table and build BFs?
		temporary_memory_state = TemporaryMemoryManager::Get(context).Register(context);
		temporary_memory_state->SetRemainingSizeAndUpdateReservation(context, op.GetMaxThreadMemory(context));
	}

	void SinkMinMaxFilter(DataChunk &chunk) const {
		for (auto &pair : col_to_expr) {
			idx_t col_idx = pair.first;
			idx_t expr_idx = pair.second;

			// min and max
			local_aggregate_state->Sink(chunk, col_idx, expr_idx, chunk.size());
			local_aggregate_state->Sink(chunk, col_idx, expr_idx + 1, chunk.size());
		}
	}

	ClientContext &client_context;
	unique_ptr<ColumnDataCollection> local_data;

	//! Local Min/Max aggregates for filter pushdown
	unordered_map<idx_t, idx_t> col_to_expr;
	unique_ptr<LocalUngroupedAggregateState> local_aggregate_state;

	//! If memory is not enough, give up creating BFs
	unique_ptr<TemporaryMemoryState> temporary_memory_state;
};

bool PhysicalCreateBF::CanAbandon(ClientContext &context, idx_t &source_rows) const {
	bool enabled = true;
	context.TryGetCurrentSetting("rpt_dynamic_abandon", enabled);
	if (!enabled || build_pipelines.size() != 1 || !this_pipeline || this_pipeline->IsExternalInput()) {
		return false;
	}
	// Exact row-id materialization is part of the late-materialization contract.
	for (auto &plan : filter_plans) {
		if (plan.logical_plan->exact_row_ids) {
			return false;
		}
	}
	auto source = this_pipeline->GetSource();
	if (!source || source->type != PhysicalOperatorType::TABLE_SCAN) {
		return false;
	}
	auto &scan = source->Cast<PhysicalTableScan>();
	if (scan.function.GetName().GetIdentifierName() != "seq_scan" || !scan.function.cardinality) {
		return false;
	}
	// Replay only deterministic, stateless operations on a native table scan.
	// Joins, aggregates, CTEs and arbitrary extension operators keep materializing.
	for (auto &ref : this_pipeline->GetIntermediateOperators()) {
		auto &op = ref.get();
		if (op.type == PhysicalOperatorType::FILTER) {
			if (op.Cast<PhysicalFilter>().expression->IsVolatile()) {
				return false;
			}
		} else if (op.type == PhysicalOperatorType::PROJECTION) {
			for (auto &expr : op.Cast<PhysicalProjection>().select_list) {
				if (expr->IsVolatile()) {
					return false;
				}
			}
		} else if (op.type != PhysicalOperatorType::EXTENSION ||
		           dynamic_cast<PhysicalUseBF *>(&op) == nullptr) {
			return false;
		}
	}
	auto cardinality = scan.function.cardinality(context, scan.bind_data.get());
	if (!cardinality || !cardinality->has_estimated_cardinality) {
		return false;
	}
	source_rows = cardinality->estimated_cardinality;
	return source_rows > 32 * STANDARD_VECTOR_SIZE;
}

void PhysicalCreateBF::RestoreConsumerPipelines() const {
	// Every consumer depends on this pipeline's finalize event, so none is running.
	// Discard the partial materialization and replay the entire scan with a fresh
	// source/local operator state. No sampled rows are emitted or dropped.
	PipelineBuildState build_state;
	for (auto &weak_consumer : consumer_pipelines) {
		auto consumer = weak_consumer.lock();
		if (!consumer || consumer->GetSource().get() != this) {
			continue;
		}
		auto operators = this_pipeline->GetIntermediateOperators();
		auto &downstream = consumer->GetIntermediateOperators();
		operators.insert(operators.end(), downstream.begin(), downstream.end());
		build_state.SetPipelineSource(*consumer, *this_pipeline->GetSource());
		// Ready() has already put both pipelines into source-to-sink order.
		build_state.SetPipelineOperators(*consumer, std::move(operators));
		consumer->ClearSource();
	}
}

SinkResultType PhysicalCreateBF::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &state = input.local_state.Cast<CreateBFLocalSinkState>();
	auto &gstate = input.global_state.Cast<CreateBFGlobalSinkState>();
	if (gstate.abandoned.load()) {
		return SinkResultType::FINISHED;
	}
	if (gstate.can_abandon && !gstate.is_selectivity_checked.load()) {
		gstate.num_input_rows.fetch_add(chunk.size());
		// A filtered chunk can retain the full scan vector allocation even
		// when only a few rows are materialized. Size the sample by its data.
		gstate.total_row_size.fetch_add(chunk.GetDataSize());
		if (gstate.input_chunks.fetch_add(1) + 1 >= 32 && !gstate.is_selectivity_checked.exchange(true)) {
			PipelineProgress progress;
			this_pipeline->GetDetailedProgress(progress);
			double fraction = progress.source.total > 0 ? progress.source.done / progress.source.total : 0;
			double rows = static_cast<double>(gstate.num_input_rows.load());
			double row_width = static_cast<double>(gstate.total_row_size.load()) / rows;
			if (fraction > 0 && fraction < 0.65) {
				gstate.observed_selectivity = rows / (fraction * static_cast<double>(gstate.source_rows));
				bool weak_filter = gstate.observed_selectivity > 0.35 ||
				                   (row_width > 40 && gstate.observed_selectivity > 0.2);
				bool memory_pressure = rows * row_width / fraction >=
				                       static_cast<double>(state.temporary_memory_state->GetReservation());
				if (weak_filter || memory_pressure) {
					gstate.abandon_reason = weak_filter ? "high pass rate" : "estimated memory";
					gstate.abandoned.store(true);
					return SinkResultType::FINISHED;
				}
			}
		}
	}

	// Bloomfilter
	state.local_data->Append(chunk);

	// min-max
	for (auto &pair : state.col_to_expr) {
		idx_t col_idx = pair.first;
		idx_t expr_idx = pair.second;

		state.local_aggregate_state->Sink(chunk, col_idx, expr_idx, chunk.size());
		state.local_aggregate_state->Sink(chunk, col_idx, expr_idx + 1, chunk.size());
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalCreateBF::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<CreateBFGlobalSinkState>();
	auto &state = input.local_state.Cast<CreateBFLocalSinkState>();

	if (gstate.abandoned.load()) {
		state.local_data.reset();
		return SinkCombineResultType::FINISHED;
	}
	// min-max aggregation
	gstate.global_aggregate_state->Combine(*state.local_aggregate_state);

	lock_guard<mutex> guard(gstate.lock);
	size_t global_size = gstate.temporary_memory_state->GetMinimumReservation() + state.local_data->SizeInBytes();
	gstate.temporary_memory_state->SetMinimumReservation(global_size);
	gstate.temporary_memory_state->SetRemainingSizeAndUpdateReservation(context.client, global_size);

	gstate.local_data_collections.push_back(std::move(state.local_data));
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
//! If we have only one thread, always finalize single-threaded.
static bool FinalizeSingleThreaded(const CreateBFGlobalSinkState &sink) {
	// if only one thread, finalize single-threaded
	const auto num_threads = NumericCast<idx_t>(sink.num_threads);
	if (num_threads == 1) {
		return true;
	}

	// if we want to verify parallelism, finalize parallel
	if (sink.context.config.verify_parallelism) {
		return false;
	}

	if (sink.data_collection->Count() < 1048576) {
		return true;
	}

	return false;
}

class CreateBFFinalizeTask : public ExecutorTask {
public:
	CreateBFFinalizeTask(shared_ptr<Event> event_p, ClientContext &context, CreateBFGlobalSinkState &sink_p,
	                     idx_t chunk_idx_from_p, idx_t chunk_idx_to_p)
	    : ExecutorTask(context, std::move(event_p), sink_p.op), sink(sink_p), chunk_idx_from(chunk_idx_from_p),
	      chunk_idx_to(chunk_idx_to_p) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		DataChunk chunk;
		sink.data_collection->InitializeScanChunk(chunk);
		for (idx_t i = chunk_idx_from; i < chunk_idx_to; i++) {
			sink.data_collection->FetchChunk(i, chunk);
			for (auto &pair : sink.op.unique_rpt_filters) {
				auto &cols_build = pair.first;
				auto &bf = pair.second->filter;
				bf->Insert(chunk, cols_build);
			}
		}
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

private:
	CreateBFGlobalSinkState &sink;
	idx_t chunk_idx_from;
	idx_t chunk_idx_to;
};

class CreateBFFinalizeEvent : public BasePipelineEvent {
public:
	CreateBFFinalizeEvent(Pipeline &pipeline_p, CreateBFGlobalSinkState &sink)
	    : BasePipelineEvent(pipeline_p), sink(sink) {
	}

	CreateBFGlobalSinkState &sink;

public:
	void Schedule() override {
		auto &context = pipeline->GetClientContext();

		vector<shared_ptr<Task>> finalize_tasks;
		auto &collection = sink.data_collection;
		const auto chunk_count = collection->ChunkCount();

		if (FinalizeSingleThreaded(sink)) {
			// Single-threaded finalize
			finalize_tasks.push_back(
			    make_uniq<CreateBFFinalizeTask>(shared_from_this(), context, sink, 0U, chunk_count));
		} else {
			// Parallel finalize
			const idx_t chunks_per_task = context.config.verify_parallelism ? 1 : CHUNKS_PER_TASK;
			for (idx_t chunk_idx = 0; chunk_idx < chunk_count; chunk_idx += chunks_per_task) {
				auto chunk_idx_to = MinValue<idx_t>(chunk_idx + chunks_per_task, chunk_count);
				finalize_tasks.push_back(
				    make_uniq<CreateBFFinalizeTask>(shared_from_this(), context, sink, chunk_idx, chunk_idx_to));
			}
		}
		SetTasks(std::move(finalize_tasks));
	}

	void FinishEvent() override {
		for (auto &pair : sink.op.unique_rpt_filters) {
			auto &bf = pair.second;
			bf->filter->SetValid();
		}
	}

	static constexpr idx_t CHUNKS_PER_TASK = 64;
};

void CreateBFGlobalSinkState::ScheduleFinalize(Pipeline &pipeline, Event &event) {
	auto new_event = make_shared_ptr<CreateBFFinalizeEvent>(pipeline, *this);
	event.InsertEvent(std::move(new_event));
}

SinkFinalizeType PhysicalCreateBF::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                            OperatorSinkFinalizeInput &input) const {
	auto &sink = input.global_state.Cast<CreateBFGlobalSinkState>();
	if (sink.abandoned.load()) {
		if (context.config.enable_profiler) {
			std::cerr << "RPT_DYNAMIC_ABANDON reason=" << sink.abandon_reason
			          << " sample_rows=" << sink.num_input_rows.load()
			          << " pass_rate=" << sink.observed_selectivity << std::endl;
		}
		// No partial filter or min/max range may be published.
		sink.local_data_collections.clear();
		RestoreConsumerPipelines();
		return SinkFinalizeType::READY;
	}

	// Finalize min-max
	sink.FinalizeMinMax();

	// Collect local data
	for (auto &local_data : sink.local_data_collections) {
		sink.data_collection->Combine(*local_data);
	}
	sink.local_data_collections.clear();
	sink.FinalizeOrFilter();

	// Initialize the bloom filter
	const auto num_rows = sink.data_collection->Count();
	const bool parallel_bitmap_build = !FinalizeSingleThreaded(sink);
	for (auto &pair : unique_rpt_filters) {
		auto &bf = pair.second;
		auto id = pair.first;
		bool exact_row_ids = false;
		for (const auto &plan : filter_plans) {
			exact_row_ids |= plan.bound_cols == id && plan.logical_plan->exact_row_ids;
		}
		if (exact_row_ids) {
			if (id.size() != 1 || !sink.val_ranges.count(id[0])) {
				throw InternalException("RPT: exact row-id filtering requires an integral range");
			}
			auto range = sink.val_ranges[id[0]];
			bf->filter = make_shared_ptr<BitmapFilter>(
			    context, BitmapFilterConfig(range.first, range.second), parallel_bitmap_build);
			continue;
		}
		if (config.use_bitmap_filter && id.size() == 1 && sink.val_ranges.count(id[0])) {
			auto range = sink.val_ranges[id[0]];
			const auto span = uint64_t(range.second) - uint64_t(range.first);
			// Compare the same cost threshold without overflowing 128 * num_rows.
			const auto min_rows = span / 128 + (span % 128 != 0);
			if (range.second < range.first || span <= 8000000 || min_rows <= num_rows) {
				bf->filter = make_shared_ptr<BitmapFilter>(
				    context, BitmapFilterConfig(range.first, range.second), parallel_bitmap_build);
			}
		}
		if (!bf->filter) {
			bf->filter = make_shared_ptr<RPTBloomFilter>(context, BloomFilterConfig(), num_rows);
		}
	}

	sink.ScheduleFinalize(pipeline, event);
	if (config.use_table_filter_bf) {
		for (idx_t idx = 0; idx < filter_plans.size(); idx++) {
			auto &dynamic_filters = min_max_to_create[idx];
			auto &applied_bindings = min_max_applied_cols[idx];
			if (!dynamic_filters || applied_bindings.size() != 1) {
				continue;
			}
			const auto &column = scan_filter_columns[idx][0];
			bool preserves_cast_errors;
			auto input_expr =
			    RuntimeFilterCastUtil::CreateRuntimeFilterInputExpression(context, column, preserves_cast_errors);
			auto null_check_input = preserves_cast_errors ? input_expr->Copy() : nullptr;
			auto type = filter_plans[idx].logical_plan->return_types[0];
			auto expr = RPTTableFilter(bf_to_create[idx]->GetFilter(), type).GetExpression(std::move(input_expr));
			expr = PreserveScanCastErrors(std::move(expr), std::move(null_check_input));
			dynamic_filters->PushFilter(*this, applied_bindings[0].column_index,
			                           make_uniq<ExpressionFilter>(std::move(expr)));
		}
	}

	return SinkFinalizeType::READY;
}

unique_ptr<GlobalSinkState> PhysicalCreateBF::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<CreateBFGlobalSinkState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalCreateBF::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<CreateBFLocalSinkState>(context.client, *this);
}

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
class CreateBFGlobalSourceState : public GlobalSourceState {
public:
	explicit CreateBFGlobalSourceState(const ColumnDataCollection &collection)
	    : max_threads(MaxValue<idx_t>(collection.ChunkCount(), 1)), data_collection(collection) {
		collection.InitializeScan(global_scan_state);
	}

	idx_t MaxThreads() override {
		return max_threads;
	}

	const idx_t max_threads;
	const ColumnDataCollection &data_collection;
	ColumnDataParallelScanState global_scan_state;
};

class CreateBFLocalSourceState : public LocalSourceState {
public:
	ColumnDataLocalScanState local_scan_state;
};

InsertionOrderPreservingMap<string> PhysicalCreateBF::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;

	result["Name"] = "PhysicalRPTCreateBF";
	result["BF Number"] = std::to_string(bf_to_create.size());
	result["ID"] = "0x" + std::to_string(reinterpret_cast<size_t>(this));

	string min_max_filter;
	for (auto &filter : min_max_applied_cols) {
		for (auto &v : filter) {
			min_max_filter += std::to_string(v.table_index.index) + "." + std::to_string(v.column_index) + " ";
		}
	}
	result["Min-Max Filter"] = min_max_filter;

	return result;
}

unique_ptr<GlobalSourceState> PhysicalCreateBF::GetGlobalSourceState(ClientContext &context) const {
	auto &gstate = sink_state->Cast<CreateBFGlobalSinkState>();
	return make_uniq<CreateBFGlobalSourceState>(*gstate.data_collection);
}

unique_ptr<LocalSourceState> PhysicalCreateBF::GetLocalSourceState(ExecutionContext &context,
                                                                   GlobalSourceState &gstate) const {
	return make_uniq<CreateBFLocalSourceState>();
}

SourceResultType PhysicalCreateBF::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                           OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<CreateBFGlobalSourceState>();
	auto &lstate = input.local_state.Cast<CreateBFLocalSourceState>();
	gstate.data_collection.Scan(gstate.global_scan_state, lstate.local_scan_state, chunk);
	return chunk.size() == 0 ? SourceResultType::FINISHED : SourceResultType::HAVE_MORE_OUTPUT;
}

void PhysicalCreateBF::BuildFilterInput(Pipeline &current, MetaPipeline &meta_pipeline) {
	if (!this_pipeline) {
		auto &child = meta_pipeline.CreateChildMetaPipeline(current, *this);
		this_pipeline = child.GetBasePipeline();
		child.Build(children[0]);
		// UNION ALL can feed the same sink from several pipelines.
		child.GetPipelines(build_pipelines, false);
	} else {
		for (auto &pipeline : build_pipelines) {
			current.AddDependency(pipeline);
		}
	}
}

void PhysicalCreateBF::BuildPipelinesFromRelated(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();

	// operator is a sink, build a pipeline
	D_ASSERT(children.size() == 1);

	BuildFilterInput(current, meta_pipeline);
}

ProgressData PhysicalCreateBF::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	auto &source_state = gstate.Cast<CreateBFGlobalSourceState>();

	ProgressData res;
	res.total = static_cast<double>(source_state.data_collection.Count());

	lock_guard<mutex> guard(source_state.global_scan_state.lock);
	res.done = static_cast<double>(source_state.global_scan_state.scan_state.current_row_index);
	return res;
}

void PhysicalCreateBF::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();

	auto &state = meta_pipeline.GetState();
	// operator is a sink, build a pipeline
	sink_state.reset();
	D_ASSERT(children.size() == 1);

	// single operator: the operator becomes the data source of the current pipeline
	state.SetPipelineSource(current, *this);
	consumer_pipelines.emplace_back(current.shared_from_this());
	BuildFilterInput(current, meta_pipeline);
}
} // namespace duckdb
