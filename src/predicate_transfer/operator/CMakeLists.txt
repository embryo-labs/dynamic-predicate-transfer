add_library_unity(
  rpt_extension_operators
  OBJECT
  logical_create_bf.cpp
  logical_use_bf.cpp
  physical_create_bf.cpp
  physical_use_bf.cpp
  logical_dynamic_select.cpp
  physical_dynamic_select.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:rpt_extension_operators>
    PARENT_SCOPE)
