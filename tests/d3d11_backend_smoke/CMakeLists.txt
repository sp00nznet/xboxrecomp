# d3d11_backend_smoke - the pushbuffer executor drawing through D3D11
#
# Feeds nv2a_pb_exec_method() a hand-written method stream (surface, clear,
# one quad, flip) with RECOMP_PB_BACKEND=d3d11, so the backend in
# src/d3d/nv2a_pb_d3d11.c can be checked under Proton without a title.
#   cmake --build build-win --target d3d11_backend_smoke

if(WIN32)
    add_executable(d3d11_backend_smoke smoke.c)
    # xbox_usb: the kernel's trapped-MMIO dispatch (xbox_VehMmioFault) calls
    # the OHCI model. xbox_kernel links it PRIVATE, but this file is read
    # (from src/d3d) before src/usb defines the target, and the mingw link
    # line came out without it; name it here.
    target_link_libraries(d3d11_backend_smoke PRIVATE xbox_kernel xbox_d3d8 xbox_video
                          xbox_usb d3d11 dxgi dxguid)
    target_include_directories(d3d11_backend_smoke PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../src/d3d)
    # Needs a D3D11 device: under ctest, a Proton wrapper as
    # CMAKE_CROSSCOMPILING_EMULATOR. Run in both visibility modes.
    enable_testing()
    add_test(NAME d3d11_backend_smoke COMMAND d3d11_backend_smoke)
    add_test(NAME d3d11_backend_smoke_occ_sync COMMAND d3d11_backend_smoke)
    set_tests_properties(d3d11_backend_smoke_occ_sync PROPERTIES
                         ENVIRONMENT RECOMP_D3D11_OCC=sync)
endif()
