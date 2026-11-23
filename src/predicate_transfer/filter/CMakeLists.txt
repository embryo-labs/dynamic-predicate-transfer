add_library_unity(rpt_extension_bloom_filter OBJECT bloom_filter.cpp
                  bitmap_filter.cpp table_filter.cpp)

set(ALL_OBJECT_FILES
    ${ALL_OBJECT_FILES} $<TARGET_OBJECTS:rpt_extension_bloom_filter>
    PARENT_SCOPE)
