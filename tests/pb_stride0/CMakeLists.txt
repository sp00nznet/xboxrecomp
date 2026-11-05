# pb_stride0 - the vertex-program path (nv2a_pb_exec.c): an array with a
# size and stride 0 is an array, so every vertex reads its one element, not
# the inline SET_VERTEX_DATA4F value; size 0 is inline. Feeds
# nv2a_pb_exec_method() a hand-written method stream on the CPU path; no GPU
# backend, so it runs on every POSIX host.
#   cmake --build <build> --target pb_stride0
add_executable(pb_stride0 test_main.c)
target_link_libraries(pb_stride0 PRIVATE xboxrecomp)
target_include_directories(pb_stride0 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_stride0 COMMAND pb_stride0)
