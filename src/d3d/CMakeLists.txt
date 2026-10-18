# Xbox D3D8 Compatibility Layer
#   _WIN32 -> backed by Direct3D 11 (d3d8_*.c, d3d8_combiners.c, d3d8_vsh.c)
#   Both   -> d3d8_vsh_parse.c, d3d8_combiners_parse.c (NV2A vertex-program
#             and register-combiner decoders) and d3d8_vsh_hlsl.c,
#             d3d8_combiners_hlsl.c (their HLSL emitters), d3d8_vsh_msl.c,
#             d3d8_combiners_msl.c (their MSL emitters): no graphics API
#   POSIX  -> backed by OpenGL 3.3 core via SDL2 + libepoxy (d3d8_gl.c)

if(WIN32)
    add_library(xbox_d3d8 STATIC
        d3d8_device.c
        d3d8_gamma.c
        d3d8_resources.c
        d3d8_shaders.c
        d3d8_states.c
        d3d8_combiners.c
        d3d8_vsh.c
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
        d3d8_vsh_hlsl.c
        d3d8_combiners_hlsl.c
        d3d8_vsh_msl.c
        d3d8_combiners_msl.c
        nv2a_pb_d3d11.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform d3d11 dxgi dxguid d3dcompiler)
    # nv2a_pb_d3d11.c's window records keys in src/input's key table.
    target_link_libraries(xbox_d3d8 PUBLIC xbox_input)
    # nv2a_pb_d3d11.c: the pushbuffer executor's state (nv2a_pb_state.h) and
    # guest memory (xbox_memory_layout.h) live with the kernel.
    target_include_directories(xbox_d3d8 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # Offline smoke test for the pure format layer (no D3D11 device needed).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
    # D3DCompile under the host's d3dcompiler_47 (Wine's, under Proton).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3dcompile_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3dcompile_smoke)
    # The pushbuffer executor's D3D11 backend, driven by a synthetic stream.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d11_backend_smoke
                     ${CMAKE_CURRENT_BINARY_DIR}/d3d11_backend_smoke)
else()
    add_library(xbox_d3d8 STATIC
        d3d8_gl.c
        d3d8_vsh_parse.c
        d3d8_combiners_parse.c
        d3d8_vsh_hlsl.c
        d3d8_combiners_hlsl.c
        d3d8_vsh_msl.c
        d3d8_combiners_msl.c
    )
    find_package(SDL2 REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EPOXY REQUIRED epoxy)
    target_include_directories(xbox_d3d8 PRIVATE ${EPOXY_INCLUDE_DIRS})
    # EPOXY_LINK_LIBRARIES, not EPOXY_LIBRARIES: the latter is the bare name
    # `epoxy`, which only resolves if EPOXY_LIBRARY_DIRS happens to be on the
    # link path. That holds for the top-level build and not when this is pulled
    # in via add_subdirectory from tests/, where the link fails with
    # "library 'epoxy' not found". The _LINK_LIBRARIES form is absolute paths.
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL2::SDL2 ${EPOXY_LINK_LIBRARIES})
    if(APPLE)
        # The pushbuffer executor's Metal backend (RECOMP_PB_BACKEND=metal).
        # Manual retain/release (no -fobjc-arc); shaders are MSL compiled at
        # runtime, so no metallib build step.
        target_sources(xbox_d3d8 PRIVATE nv2a_pb_metal.m)
        target_include_directories(xbox_d3d8 PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../kernel ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(xbox_d3d8 PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework Foundation")
        # The executor's backend, driven by a synthetic stream: Metal against
        # the CPU rasteriser, frame by frame.
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/nv2a_backend_smoke
                         ${CMAKE_CURRENT_BINARY_DIR}/nv2a_backend_smoke)
    endif()
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The CPU rasteriser's texture cache, driven by a synthetic stream (POSIX:
# the Proton test set does not run it).
if(NOT WIN32)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pb_tex_cache
                     ${CMAKE_CURRENT_BINARY_DIR}/pb_tex_cache)
    # And its vertex-program path's near clip and border colour.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pb_clip_border
                     ${CMAKE_CURRENT_BINARY_DIR}/pb_clip_border)
    # And a stride-0 vertex array: every vertex reads its one element.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pb_stride0
                     ${CMAKE_CURRENT_BINARY_DIR}/pb_stride0)
    # And the semaphore release's context DMA base (RAMHT/PRAMIN lookup).
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pb_semaphore
                     ${CMAKE_CURRENT_BINARY_DIR}/pb_semaphore)
    # And PGRAPH 0x400B10 from the executor's releases or a fence mirror.
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pgraph_fence
                     ${CMAKE_CURRENT_BINARY_DIR}/pgraph_fence)
endif()

# The HLSL the parse -> generate pipeline emits, against recorded hashes
# (every host: the emitters are API-free).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_hlsl_split
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_hlsl_split)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()
# The MSL emitters' (d3d8_msl.h) hash test; it builds on every host, and on
# macOS also compiles each case with Metal.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_msl_split
                 ${CMAKE_CURRENT_BINARY_DIR}/d3d8_msl_split)
