# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input)
if(WIN32)
    # gpu_present.c: the framebuffer window's D3D11 presenter.
    target_sources(xbox_video PRIVATE gpu_present.c)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32
                          d3d11 dxgi dxguid d3dcompiler)
else()
    # The framebuffer window on POSIX hosts: SDL2, on the process main thread
    # (fb_present_sdl.c), which then also pumps the events the game
    # controllers are read from.
    find_package(SDL2 REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2)
endif()
