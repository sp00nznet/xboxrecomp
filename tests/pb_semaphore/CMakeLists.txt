# pb_semaphore - BACK_END_WRITE_SEMAPHORE_RELEASE and GET_REPORT write at
# their context DMA's base plus the offset, within its limit, the base and
# limit read from RAMHT and PRAMIN in the NV2A aperture when the object is
# bound (nv2a_pb_exec.c, ctxdma_bind). Feeds
# nv2a_pb_exec_method() a hand-written stream on the CPU path; no GPU
# backend, so it runs on every POSIX host.
#   cmake --build <build> --target pb_semaphore
add_executable(pb_semaphore test_main.c)
target_link_libraries(pb_semaphore PRIVATE xboxrecomp)
target_include_directories(pb_semaphore PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_semaphore COMMAND pb_semaphore)
