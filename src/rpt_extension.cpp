#define DUCKDB_EXTENSION_MAIN

#include "include/rpt_extension.hpp"
#include "include/predicate_transfer/config.hpp"
#include "include/predicate_transfer/operator/logical_create_bf.hpp"
#include "include/predicate_transfer/operator/logical_use_bf.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

namespace duckdb {

// These operators only deserialize plans; they do not bind SQL statements.
static BoundStatement DeclineBind(ClientContext &, Binder &, OperatorExtensionInfo *, SQLStatement &) {
	return BoundStatement();
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	Connection con(db);

	// add a parser extension
	auto &config = DBConfig::GetConfig(db);
	config.AddExtensionOption("rpt_enabled", "Enable RPT predicate transfer", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
	config.AddExtensionOption("rpt_largest_root", "Order transfer roots by descending cardinality",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("rpt_estimated_cost_order", "Order transfer roots by estimated scan cost",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("rpt_skip_unfiltered", "Prune transfers from unfiltered tables", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
	config.AddExtensionOption("rpt_late_materialization", "Use late materialization for transfer inputs",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("rpt_dynamic_in_filter", "Push small IN filters into scans", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false));
	config.AddExtensionOption("rpt_extra_transfer", "Add preliminary transfers from small filtered tables",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(false));
	config.AddExtensionOption("rpt_table_filter", "Push transfer filters into table scans", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
	config.AddExtensionOption("rpt_row_id_filter", "Use row-id filtering for late materialization",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.AddExtensionOption("rpt_bitmap_filter", "Use bitmaps for bounded integral keys", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(true));
	config.AddExtensionOption("rpt_dynamic_abandon", "Abandon unselective filter construction and replay the input pipeline",
	                          LogicalType::BOOLEAN, Value::BOOLEAN(true));
	config.GetCallbackManager().Register(RptExtension());
	auto create_extension = make_uniq<LogicalCreateBF::OperatorExtension>();
	create_extension->Bind = DeclineBind;
	config.GetCallbackManager().Register(std::move(create_extension));
	auto use_extension = make_uniq<LogicalUseBF::OperatorExtension>();
	use_extension->Bind = DeclineBind;
	config.GetCallbackManager().Register(std::move(use_extension));
}

void RptExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

void RptExtension::RptPreOptimizeFunction(OptimizerExtensionInput &input, duckdb::unique_ptr<LogicalOperator> &plan) {
}

void RptExtension::RptOptimizeFunction(OptimizerExtensionInput &input, duckdb::unique_ptr<LogicalOperator> &plan) {
	bool enabled = true;
	input.context.TryGetCurrentSetting("rpt_enabled", enabled);
	if (!enabled) {
		return;
	}
	RPTOptimizerConfig config;
	input.context.TryGetCurrentSetting("rpt_largest_root", config.use_largest_root_order_flag);
	input.context.TryGetCurrentSetting("rpt_estimated_cost_order", config.use_estimated_cost_order_flag);
	input.context.TryGetCurrentSetting("rpt_skip_unfiltered", config.skip_unfiltered_table_flag);
	input.context.TryGetCurrentSetting("rpt_late_materialization", config.late_materialize_flag);
	input.context.TryGetCurrentSetting("rpt_dynamic_in_filter", config.use_dynamic_in_filter);
	input.context.TryGetCurrentSetting("rpt_extra_transfer", config.use_extra_transfer);
	input.context.TryGetCurrentSetting("rpt_table_filter", config.use_table_filter_bf);
	input.context.TryGetCurrentSetting("rpt_row_id_filter", config.use_row_id_filter);
	input.context.TryGetCurrentSetting("rpt_bitmap_filter", config.use_bitmap_filter);
	PredicateTransferOptimizer op(input.optimizer, input.context, config);
	plan = op.PreOptimize(std::move(plan));
	plan = op.Optimize(std::move(plan));
	if (input.context.config.enable_profiler) {
		std::cerr << op.GetDebugInfo() << std::endl;
	}
}

std::string RptExtension::Name() const {
	return "rpt";
}

std::string RptExtension::Version() const {
#ifdef EXT_VERSION_RPT
	return EXT_VERSION_RPT;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(rpt, loader) {
	duckdb::LoadInternal(loader);
}
}
