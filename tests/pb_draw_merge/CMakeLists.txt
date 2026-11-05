# pb_draw_merge - draw merging in the walker (nv2a_pb_exec.c, nv2a_merge):
# back-to-back triangle batches with nothing between them reach a GPU
# backend as one triangle list, with the triangles each would have drawn,
# and any other method or the walk's end draws the chain. Feeds
# nv2a_pb_exec_method() a hand-written method stream into a recording
# backend; no GPU, so it runs on every POSIX host.
#   cmake --build <build> --target pb_draw_merge
add_executable(pb_draw_merge test_main.c)
target_link_libraries(pb_draw_merge PRIVATE xboxrecomp)
target_include_directories(pb_draw_merge PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_draw_merge COMMAND pb_draw_merge)
