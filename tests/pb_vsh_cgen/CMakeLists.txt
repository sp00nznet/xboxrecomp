# pb_vsh_cgen - nv2a_pb_vsh_consts_gen (nv2a_pb_exec.c): every method that
# writes the vertex constants c[] moves it, and other methods leave it, so a
# GPU backend may skip its per-batch c[] compare while it is unchanged. Feeds
# nv2a_pb_exec_method() a hand-written method stream; no GPU backend, so it
# runs on every POSIX host.
#   cmake --build <build> --target pb_vsh_cgen
add_executable(pb_vsh_cgen test_main.c)
target_link_libraries(pb_vsh_cgen PRIVATE xboxrecomp)
target_include_directories(pb_vsh_cgen PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_vsh_cgen COMMAND pb_vsh_cgen)
