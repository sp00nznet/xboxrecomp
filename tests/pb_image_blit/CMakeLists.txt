# pb_image_blit - the NV2A 2D engine (nv2a_2d.c): NV062 CONTEXT_SURFACES_2D
# and NV09F IMAGE_BLIT decode, the blit's extent and host-texel box, the
# guest-memory copy, and the executor's path from SET_OBJECT and the
# context DMA objects to a blit (CPU path, and a stub backend's on_blit).
# No GPU backend, so it runs on every POSIX host.
#   cmake --build <build> --target pb_image_blit
add_executable(pb_image_blit test_main.c)
target_link_libraries(pb_image_blit PRIVATE xboxrecomp)
target_include_directories(pb_image_blit PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_image_blit COMMAND pb_image_blit)
