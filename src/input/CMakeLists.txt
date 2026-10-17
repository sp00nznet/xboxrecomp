# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard.c          # the key table and the one key map (every host)
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The mapping rules (bindings, remapping, deadzones), also buildable alone;
# keyboard.c resolves its key map through them.
add_subdirectory(mapping)
target_link_libraries(xbox_input PUBLIC xbox_input_map)

if(WIN32)
    # Mouse+keyboard pad (RECOMP_MKB) reads Win32 raw input.
    target_sources(xbox_input PRIVATE mkb_input.c)
    target_link_libraries(xbox_input PUBLIC platform xinput)
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
