# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    window_title.c
    fb_window_wanted.c
)
# The framebuffer window: GDI on Windows; on POSIX an SDL3 window that owns
# the process main thread (fb_present_sdl.c, xbox_HostWindowMain), which then
# also pumps the events the game controllers are read from.
if(WIN32)
    target_sources(xbox_video PRIVATE fb_present.c)
    # The GDI window records keys in src/input's key table.
    target_link_libraries(xbox_video PUBLIC xbox_input)
else()
    find_package(SDL3 3.2 CONFIG REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL3::SDL3 xbox_input)
    if(APPLE)
        # The window's CAMetalLayer present for the Metal backend (manual
        # retain/release, like nv2a_pb_metal.m).
        target_sources(xbox_video PRIVATE fb_present_metal.m)
        target_link_libraries(xbox_video PUBLIC
            "-framework Metal" "-framework QuartzCore" "-framework CoreGraphics"
            "-framework Foundation" "-framework AppKit")
        if(XBOXRECOMP_ENHANCE)
            # present.shader's Metal chain (librashader.h, loaded at run time).
            target_include_directories(xbox_video PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/librashader)
        endif()
    endif()
endif()
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
# xbox_nv2a: the window scans out the PVIDEO overlay (nv2a_mmio_hook.c).
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input xbox_nv2a)
if(WIN32)
    # gpu_present.c: the framebuffer window's D3D11 presenter.
    target_sources(xbox_video PRIVATE gpu_present.c)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32
                          d3d11 dxgi dxguid d3dcompiler)
endif()
