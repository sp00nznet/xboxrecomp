# d3d8_msl_split - the MSL d3d8_vsh_msl.c and d3d8_combiners_msl.c emit for
# d3d8_hlsl_split's pseudo-random programs and combiner registers, hashed and
# checked against expected.h; on macOS every case is also compiled with Metal.
#   cmake --build <build> --target d3d8_msl_split
# MSL_DUMP=<file> writes the text, MSL_EXPECT_OUT=<file> a new expected.h.

add_executable(d3d8_msl_split msl_split.c)
target_link_libraries(d3d8_msl_split PRIVATE xbox_d3d8)
if(APPLE)
    target_sources(d3d8_msl_split PRIVATE msl_compile.m)
    target_compile_definitions(d3d8_msl_split PRIVATE MSL_HAVE_METAL=1)
    target_link_libraries(d3d8_msl_split PRIVATE "-framework Metal" "-framework Foundation")
endif()
enable_testing()
add_test(NAME d3d8_msl_split COMMAND d3d8_msl_split)
