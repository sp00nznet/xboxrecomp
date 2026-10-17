# Enhancements layer (opt-in: XBOXRECOMP_ENHANCE=ON in the root CMakeLists).
#   recomp_cfg.c   TOML-subset reader, no dependencies (C11 + libc)
#   enhance_cfg.c  the layering: env > title config.toml > config.toml >
#                  config.default.toml beside the executable > default
#   enhance.c      xbox_enhance_init: the toolkit's keys into nv2a_host_opts
#   enhance_input.c the [input] keys into src/input (keyboard and mouse)
# docs/runtime/config.md has the file format and the keys.
#
# The layer links the runtime (xbox_kernel, for nv2a_host_opts_set), never
# the other way round: with the layer off the options stay zero (stock).

add_library(xbox_enhance STATIC recomp_cfg.c enhance_cfg.c enhance.c enhance_input.c)
target_include_directories(xbox_enhance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_enhance PUBLIC recomp_env xbox_kernel xbox_input)
if(NOT MSVC)
    target_compile_options(xbox_enhance PRIVATE -Wall -Wextra)
endif()
