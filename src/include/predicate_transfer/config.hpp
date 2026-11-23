#pragma once

namespace duckdb {

class RPTOptimizerConfig {
public:
  bool use_largest_root_order_flag = false;
  bool use_estimated_cost_order_flag = false;
  bool skip_unfiltered_table_flag = true;
  bool late_materialize_flag = false;
  bool use_dynamic_in_filter = false;
  bool use_extra_transfer = false;
  bool use_table_filter_bf = true;
  bool use_row_id_filter = true;
  bool use_bitmap_filter = true;
};

}