# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard.c          # the key table and the one key map (every host)
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    # Mouse+keyboard pad (RECOMP_MKB) reads Win32 raw input.
    target_sources(xbox_input PRIVATE mkb_input.c)
    # mingw-w64's libxinput.a imports XINPUT1_3.dll, the DirectX June 2010
    # redistributable: stock Windows 10/11 lacks it and the exe dies with
    # 0xC0000135 before main. Proton ships it, so only native Windows shows
    # this. XINPUT1_4.dll is part of Windows since 8. The Windows SDK's
    # xinput.lib already imports 1_4 and the SDK has no xinput1_4.lib, so
    # MSVC keeps the plain name. tests/input_xinput_imports checks the result.
    target_link_libraries(xbox_input PUBLIC platform
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>)
else()
    find_package(SDL2 REQUIRED)
    # The SDL window's key and focus events into that table.
    target_sources(xbox_input PRIVATE keyboard_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
