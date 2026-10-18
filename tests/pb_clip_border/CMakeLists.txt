# pb_clip_border - the CPU rasteriser's vertex-program path (nv2a_pb_exec.c):
# the near plane clips with the depth test off too, and texture address
# mode 4 samples SET_TEXTURE_BORDER_COLOR. Feeds nv2a_pb_exec_method() a
# hand-written method stream on the CPU path; no GPU backend, so it runs on
# every POSIX host.
#   cmake --build <build> --target pb_clip_border
add_executable(pb_clip_border test_main.c)
target_link_libraries(pb_clip_border PRIVATE xboxrecomp)
target_include_directories(pb_clip_border PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d
    ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
enable_testing()
add_test(NAME pb_clip_border COMMAND pb_clip_border)
