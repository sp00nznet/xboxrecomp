# d3dcompile_smoke - does D3DCompile work where the game runs?
#
# Under Proton that is Wine's d3dcompiler_47 feeding DXVK. Build with
#   cmake --build build-win --target d3dcompile_smoke
# and run it under the same prefix as the game; see smoke.c.

if(WIN32)
    # nv2a_backend_common.c: the depth state translation (nv2a_depth_bias,
    # nv2a_wdepth_consts_fill) the W and polygon offset cases check,
    # without the rest of the kernel.
    add_executable(d3dcompile_smoke smoke.c
        ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/nv2a_backend_common.c
        ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel/nv2a_tex_pack.c)
    target_include_directories(d3dcompile_smoke PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../src/kernel)
    target_link_libraries(d3dcompile_smoke PRIVATE xbox_d3d8 d3d11 dxgi dxguid)
endif()
