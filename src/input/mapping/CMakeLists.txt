# The mapping rules (input_map.c): bindings over keyboard.h's key table,
# pad remapping, deadzones and merging. Its own target so a port, or
# tests/input_map, can build it without SDL or the rest of the runtime;
# xbox_input links it, as keyboard.c resolves its key map through it.
add_library(xbox_input_map STATIC ../input_map.c)
target_include_directories(xbox_input_map PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/..          # input_map.h, keyboard.h, xinput_xbox.h
    ${CMAKE_CURRENT_LIST_DIR}/../..)      # platform/xbox_winnt.h
# PRIVATE: xbox_input links this, and a C standard set here must not reach
# the targets (and game code) that link xbox_input.
target_compile_features(xbox_input_map PRIVATE c_std_11)
if(MSVC)
    target_compile_options(xbox_input_map PRIVATE /W4)
else()
    target_compile_options(xbox_input_map PRIVATE -Wall -Wextra)
endif()
if(NOT MSVC)
    # sqrt, for the radial deadzone.
    target_link_libraries(xbox_input_map PRIVATE m)
endif()
