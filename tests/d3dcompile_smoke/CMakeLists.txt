# d3dcompile_smoke - does D3DCompile work where the game runs?
#
# Under Proton that is Wine's d3dcompiler_47 feeding DXVK. Build with
#   cmake --build build-win --target d3dcompile_smoke
# and run it under the same prefix as the game; see smoke.c.

if(WIN32)
    add_executable(d3dcompile_smoke smoke.c)
    target_link_libraries(d3dcompile_smoke PRIVATE xbox_d3d8 d3d11 dxgi dxguid)
endif()
