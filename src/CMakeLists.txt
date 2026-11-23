add_subdirectory(predicate_transfer)
include_directories(include)

add_library_unity(rpt_extension_src OBJECT rpt_extension.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:rpt_extension_src>
    PARENT_SCOPE)
