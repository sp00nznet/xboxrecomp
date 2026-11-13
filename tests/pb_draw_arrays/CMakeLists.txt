# pb_draw_arrays - NV097_DRAW_ARRAYS runs expand into the batch's index list
# (nv2a_pb_exec.c): the bulk fill stores what the old per-index
# loop did, up to NV_MAX_INDICES, flags the cut batch once, and truncates and
# counts runs past 0xFFFF as before. Feeds nv2a_pb_exec_method() a
# hand-written method stream into a recording backend; no GPU, so it runs on
# every POSIX host.
#   cmake --build <build> --target pb_draw_arrays
add_executable(pb_draw_arrays test_main.c)
target_link_libraries(pb_draw_arrays PRIVATE xboxrecomp)
target_include_directories(pb_draw_arrays PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_draw_arrays COMMAND pb_draw_arrays)
set_tests_properties(pb_draw_arrays PROPERTIES ENVIRONMENT "RECOMP_DEBUG=")
