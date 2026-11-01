# Enhancements layer (opt-in: XBOXRECOMP_ENHANCE=ON in the root CMakeLists).
#   recomp_cfg.c   TOML-subset reader, no dependencies (C11 + libc)
#   enhance_cfg.c  the layering: env > title config.toml > config.toml >
#                  config.default.toml beside the executable > default
#   enhance.c      xbox_enhance_init: the toolkit's keys into nv2a_host_opts
#   enhance_input.c the [input] keys into src/input (keyboard and mouse),
#                  and a mod's input state and capture
#   enhance_actions.c host actions: the registry, [input.actions], the pad
#                  filter and the flip tick (enhance_actions.h)
#   mod.c          the mods: the title's table, the mod API, the frame and
#                  batch-filter dispatch, descriptors and their settings (mod.h)
#   hud_anchor.c   the HUD layer: a mod's HUD table, elements moved to the
#                  wide frame's edges (hud_anchor.h)
#   hud_capture.c  the HUD mapping capture (RECOMP_DEBUG=hud_capture,
#                  hud_capture.h)
#   mod_describe.c the descriptor dump (RECOMP_MODS_DESCRIBE=json)
#   enhance_menu.c the in-game menu (menu.enabled): items, pages, Save
#                  (enhance_menu.h)
#   shader_profile.c shader profiles: the profile files, the composer (one
#                  .slangp from a chain of presets; shader_profile.h)
#   shader_profile_req.c a profile as the chain's request, the start's
#                  present.profile / present.shader choice
#   shader_profile_edit.c the Shaders page's profiles: picker, chain edits,
#                  Save (shader_profile_edit.h)
#   file_browser.c the in-overlay file browser's model: roots, listing, name
#                  checks, import cap, atomic writes (file_browser.h)
# docs/runtime/config.md has the file format and the keys.
#
# The layer links the runtime (xbox_kernel, for nv2a_host_opts_set), never
# the other way round: with the layer off the options stay zero (stock).

add_library(xbox_enhance STATIC recomp_cfg.c enhance_cfg.c enhance.c enhance_input.c
    enhance_actions.c mod.c hud_anchor.c hud_capture.c mod_describe.c enhance_menu.c
    shader_profile.c shader_profile_req.c shader_profile_edit.c file_browser.c)
target_include_directories(xbox_enhance PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xbox_enhance PUBLIC recomp_env xbox_kernel xbox_input)
if(NOT MSVC)
    target_compile_options(xbox_enhance PRIVATE -Wall -Wextra)
endif()
