cmake_minimum_required(VERSION 3.20)
project(nv2a_combiner_regression C)
enable_testing()
add_executable(nv2a-combiner-test nv2a_combiner_test.c ../../src/kernel/nv2a_combiner.c)
target_include_directories(nv2a-combiner-test PRIVATE ../../src/kernel)
# The evaluator and the shared parser against their previous versions.
add_executable(nv2a-combiner-fuzz nv2a_combiner_fuzz.c
    ../../src/kernel/nv2a_combiner.c ../../src/d3d/d3d8_combiners_parse.c
    oracle/pb_exec_rc_frozen.c oracle/d3d8_combiners_parse_frozen.c)
target_include_directories(nv2a-combiner-fuzz PRIVATE ../../src/kernel ../../src/d3d)
if(NOT WIN32)
    target_link_libraries(nv2a-combiner-test PRIVATE m)
    target_link_libraries(nv2a-combiner-fuzz PRIVATE m)
endif()
add_test(NAME nv2a-register-combiners COMMAND nv2a-combiner-test)
add_test(NAME nv2a-combiner-fuzz COMMAND nv2a-combiner-fuzz)
