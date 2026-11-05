# basic_shaders - the backends' basic shaders as text (nv2a_basic_hlsl.h,
# nv2a_basic_msl.h): the stock texts are locked by checksum, since a stock
# run must draw with the exact base shaders (render.snap_2d off),
# and render.snap_2d's variants carry their gradient sampling while the
# stock ones do not. On macOS both MSL libraries are compiled with Metal.
# The HLSL variants compile in d3dcompile_smoke (Windows) only.
#   cmake --build <build> --target basic_shaders
add_executable(basic_shaders test_main.c)
target_link_libraries(basic_shaders PRIVATE xbox_d3d8)
if(APPLE)
    target_sources(basic_shaders PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../d3d8_msl_split/msl_compile.m)
    target_compile_definitions(basic_shaders PRIVATE MSL_HAVE_METAL=1)
    target_link_libraries(basic_shaders PRIVATE "-framework Metal" "-framework Foundation")
endif()
enable_testing()
add_test(NAME basic_shaders COMMAND basic_shaders)
