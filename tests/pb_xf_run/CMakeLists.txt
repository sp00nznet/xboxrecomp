# pb_xf_run - nv2a_pb_exec_run (nv2a_pb_exec.c): the walk executes a header's
# transform program and constant uploads as one run, and must leave the same
# prog[], c[], load pointers, counters and c[] generation as the word-by-word
# path. Walks a hand-written pushbuffer in guest memory; no GPU backend, so it
# runs on every POSIX host.
#   cmake --build <build> --target pb_xf_run
add_executable(pb_xf_run test_main.c)
target_link_libraries(pb_xf_run PRIVATE xboxrecomp)
target_include_directories(pb_xf_run PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_xf_run COMMAND pb_xf_run)
