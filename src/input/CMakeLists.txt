# Xbox Input → XInput Compatibility Layer
#   pad_map.c       the host pad -> Duke mapping table and pure helpers
#   input_core.c    public API, connection cache, probe backoff
#   xinput_device.c host backends (XInput on Windows, SDL3 elsewhere)
#   keyboard.c      the key table, the compiled-in key map, the runtime
#                   bindings and the keyboard-and-mouse composer (every host)
#   mouse.c         the mouse: motion as a stick, buttons, capture (every
#                   host; its Win32 half holds the window side)
#   input_names.c   the binding vocabulary: key, mouse and control names
#   kbm_script.c    RECOMP_DEBUG=kbm_script, injected key and mouse events
#   keyboard_sdl.c  the SDL window's key events into that table (POSIX)
#   mouse_sdl.c     the SDL window's mouse events and relative mode (POSIX)
#   text_input.c    typed text for the menu's fields: the ring and the
#                   request (every host; its Win32 half reads WM_CHAR)
#   text_input_sdl.c SDL_EVENT_TEXT_INPUT and SDL's text input (POSIX)
#   pointer.c       the absolute pointer for the menu's mouse: a seqlocked
#                   snapshot, Win32 and SDL formats (every host)

add_library(xbox_input STATIC
    pad_map.c
    input_core.c
    xinput_device.c
    keyboard.c
    mouse.c
    input_names.c
    kbm_script.c
    text_input.c
    pointer.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The mapping rules (bindings, remapping, deadzones), also buildable alone;
# keyboard.c resolves its key map through them.
add_subdirectory(mapping)
target_link_libraries(xbox_input PUBLIC xbox_input_map)

if(WIN32)
    # mingw-w64's libxinput.a imports XINPUT1_3.dll, the DirectX June 2010
    # redistributable: stock Windows 10/11 lacks it and the exe dies with
    # 0xC0000135 before main. Proton ships it, so only native Windows shows
    # this. XINPUT1_4.dll is part of Windows since 8. The Windows SDK's
    # xinput.lib already imports 1_4 and the SDK has no xinput1_4.lib, so
    # MSVC keeps the plain name. tests/input_xinput_imports checks the result.
    target_link_libraries(xbox_input PUBLIC platform
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>)
else()
    find_package(SDL3 3.2 CONFIG REQUIRED)
    target_sources(xbox_input PRIVATE keyboard_sdl.c mouse_sdl.c text_input_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3 m)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()

# The mapping table, deadzone and connection cache, against a fake backend
# (every host, no device needed).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/pad_map
                 ${CMAKE_CURRENT_BINARY_DIR}/pad_map)
# The key map and its README table (every host, no SDL), and the SDL key
# and focus rules against synthetic events (POSIX).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard
                 ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard)
# The key map through the mapping rules, against a frozen copy of the old
# resolver, and the Win32 key and mouse message decoding (every host).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard_map
                 ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard_map)
# The names, the bindings, the composer, the mouse model and capture, the
# injector and the port-0 merge (every host, no SDL, no window).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_bindings
                 ${CMAKE_CURRENT_BINARY_DIR}/input_bindings)
# Beside the SDL key test: game controller hot-plug through SDL's virtual
# joysticks (POSIX).
if(NOT WIN32)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_keyboard_sdl
                     ${CMAKE_CURRENT_BINARY_DIR}/input_keyboard_sdl)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_hotplug_sdl
                     ${CMAKE_CURRENT_BINARY_DIR}/input_hotplug_sdl)
endif()
# The text ring: UTF-8 and UTF-16, control characters, overflow, the gate,
# the request and two threads (every host, no SDL, no window).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_text
                 ${CMAKE_CURRENT_BINARY_DIR}/input_text)
# The pointer snapshot: both writers' formats, the wheel's rest, the
# seqlock against a writer thread (every host, no SDL, no window).
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_pointer
                 ${CMAKE_CURRENT_BINARY_DIR}/input_pointer)
# The XInput DLL a game's exe imports must ship with Windows (Windows builds
# with an objdump to read the import table).
if(WIN32 AND CMAKE_OBJDUMP)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/input_xinput_imports
                     ${CMAKE_CURRENT_BINARY_DIR}/input_xinput_imports)
endif()
