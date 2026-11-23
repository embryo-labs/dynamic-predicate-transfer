include_directories(../include)

add_subdirectory(filter)
add_subdirectory(operator)

add_library_unity(
  rpt_extension_predicate_transfer
  OBJECT
  transfer_graph_manager.cpp
  dag.cpp
  predicate_transfer_optimizer.cpp
  table_operator_manager.cpp
  join_graph/table_operator_manager.cpp
  join_graph/join_graph.cpp
  transfer_bf_linker.cpp
  late_materialization.cpp
  reorder_mark_join.cpp
  push_bf_into_table.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:rpt_extension_predicate_transfer>
    PARENT_SCOPE)
