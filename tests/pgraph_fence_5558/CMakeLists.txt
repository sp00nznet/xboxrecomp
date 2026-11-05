# pgraph_fence_5558 - PGRAPH 0x400B10's fence field sits where D3D's fence
# methods put it: bits 2..6 for XDK 5849 and before any fence, bits 0..4 once
# an XDK 5558 pair (subchannel 5 method 0x0310, then the semaphore release)
# has been seen (nv2a_pb_exec.c fence_shift_learn, xbox_memory_layout.c
# xbox_Nv2aPgraphFenceTick). Its own process: the learned shift is global.
#   cmake --build <build> --target pgraph_fence_5558
add_executable(pgraph_fence_5558 test_main.c)
target_link_libraries(pgraph_fence_5558 PRIVATE xboxrecomp)
target_include_directories(pgraph_fence_5558 PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pgraph_fence_5558 COMMAND pgraph_fence_5558)
