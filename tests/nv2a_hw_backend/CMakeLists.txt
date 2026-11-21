cmake_minimum_required(VERSION 3.20)
project(nv2a_hw_backend C)
enable_testing()
# The D3D11 renderer (src/video/nv2a_hw_d3d11.c) against the executor's own CPU
# rasteriser: each scene is drawn both ways into guest memory and compared.
# Win32 only (D3D11); runs on WARP, so no GPU is needed.
if(WIN32)
    add_executable(nv2a-hw-backend-test nv2a_hw_backend_test.c
        ../../src/kernel/nv2a_pb_exec.c
        ../../src/kernel/nv2a_vsh_interp.c
        ../../src/kernel/nv2a_combiner.c
        ../../src/kernel/nv2a_flip_hold.c
        ../../src/video/nv2a_hw_d3d11.c)
    target_include_directories(nv2a-hw-backend-test PRIVATE ../../src/kernel ../../src ../../src/video)
    target_link_libraries(nv2a-hw-backend-test PRIVATE d3d11 dxgi dxguid d3dcompiler)
    if(MSVC)
        target_compile_definitions(nv2a-hw-backend-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME nv2a-hw-backend COMMAND nv2a-hw-backend-test)
endif()
