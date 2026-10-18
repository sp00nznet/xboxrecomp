# pgraph_fence - PGRAPH 0x400B10 bits 2..6 follow the completed fence: the
# executor's last semaphore release while no fence mirror is registered, a
# registered mirror's completed word once one is, and nothing in between
# (xbox_memory_layout.c, fence_completed and xbox_Nv2aPgraphFenceTick).
# Feeds nv2a_pb_exec_method() releases on the CPU path and ticks the register
# by hand; no GPU backend and no ack loop, so it runs on every POSIX host.
#   cmake --build <build> --target pgraph_fence
add_executable(pgraph_fence test_main.c)
target_link_libraries(pgraph_fence PRIVATE xboxrecomp)
target_include_directories(pgraph_fence PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pgraph_fence COMMAND pgraph_fence)
