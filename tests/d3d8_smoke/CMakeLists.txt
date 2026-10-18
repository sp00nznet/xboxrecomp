# d3d8_smoke - offline smoke test for the pure xbox_d3d8 format layer.
# Compiles the real d3d8_resources.c + d3d8_swizzle.h with lightweight device
# accessor stubs so the format tables, predicates, swizzle classification and
# software conversions can be validated without a D3D11 device.

if(WIN32)
    add_executable(d3d8_gamma test_gamma.c)
    target_link_libraries(d3d8_gamma PRIVATE xbox_d3d8)

    add_executable(d3d8_smoke
        ${CMAKE_CURRENT_SOURCE_DIR}/test_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d/d3d8_resources.c
    )
    target_include_directories(d3d8_smoke PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d
    )
    target_link_libraries(d3d8_smoke PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
    if(MSVC)
        target_compile_options(d3d8_smoke PRIVATE /W3)
    endif()

    # Exercise bound state objects on D3D11's software device (no window/GPU).
    add_executable(d3d8_states_smoke
        test_states.c
        ../../src/d3d/d3d8_states.c
    )
    target_include_directories(d3d8_states_smoke PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_states_smoke PRIVATE platform d3d11)

    add_executable(d3d8_a8
        test_a8.c
        ../../src/d3d/d3d8_resources.c
        ../../src/d3d/d3d8_shaders.c
        ../../src/d3d/d3d8_combiners.c
        ../../src/d3d/d3d8_combiners_parse.c
        ../../src/d3d/d3d8_combiners_hlsl.c
    )
    target_include_directories(d3d8_a8 PRIVATE ../../src/d3d)
    target_link_libraries(d3d8_a8 PRIVATE platform d3d11 dxgi dxguid d3dcompiler)
endif()
