# pb_tex_cache - the CPU rasteriser's decoded-texture cache (nv2a_pb_exec.c,
# tc_bind): one entry per texture whatever the level count a stage samples,
# and a rewrite of any level the entry holds is seen. Feeds
# nv2a_pb_exec_method() a hand-written method stream on the CPU path; no GPU
# backend, so it runs on every POSIX host.
#   cmake --build <build> --target pb_tex_cache
add_executable(pb_tex_cache test_main.c)
target_link_libraries(pb_tex_cache PRIVATE xboxrecomp)
target_include_directories(pb_tex_cache PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_tex_cache COMMAND pb_tex_cache)
