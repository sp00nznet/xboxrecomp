cmake_minimum_required(VERSION 3.20)
project(nv2a_vsh_hlsl C)
enable_testing()
# nv2a_vsh_emit_hlsl against nv2a_vsh_run: every program runs on the CPU and as
# a D3D11 vertex shader (stream output), and the outputs are compared. Win32
# only (D3D11). WARP by default so the result does not depend on the machine's
# GPU; `--hardware` runs it on the first hardware adapter instead.
if(WIN32)
    add_executable(nv2a-vsh-hlsl-test nv2a_vsh_hlsl_test.c ../../src/kernel/nv2a_vsh_interp.c)
    target_include_directories(nv2a-vsh-hlsl-test PRIVATE ../../src/kernel)
    target_link_libraries(nv2a-vsh-hlsl-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
    if(MSVC)
        target_compile_definitions(nv2a-vsh-hlsl-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME nv2a-vsh-hlsl COMMAND nv2a-vsh-hlsl-test)
endif()
