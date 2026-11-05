cmake_minimum_required(VERSION 3.20)

# ============================================================================
# YOUR_GAME_NAME - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(your_game_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Off Windows, xbox_kernel links OpenSSL::Crypto PUBLIC but finds it inside its
# own subdirectory, where the imported target stays invisible to this
# executable ("target not found" for OpenSSL::Crypto). Finding it here first
# puts it in scope for both.
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

# WIN32 selects the GUI subsystem on Windows and is ignored elsewhere, so the
# macOS build is a plain console executable.
add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# The toolkit's opt-in enhancements layer (render scale, present filter,
# fullscreen; config/config.toml over config.default.toml beside the
# executable). Configure with -DXBOXRECOMP_ENHANCE=ON to build it; main.c's
# host_enhance_init then loads it, and without it main.c builds as before.
if(TARGET xbox_enhance)
    target_link_libraries(${PROJECT_NAME} PRIVATE xbox_enhance)
endif()

# The option defaults the layer reads beside the executable, once the game
# has a packaging/config.default.toml (the template ships none). Copied at
# build time, as a file that depends on packaging/config.default.toml, so a
# developer build has the defaults layer and an edit of the file refreshes
# it; a bundle stages the file itself (xbr package).
if(EXISTS ${CMAKE_SOURCE_DIR}/packaging/config.default.toml)
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/config.default.toml
        COMMAND ${CMAKE_COMMAND} -E copy
                ${CMAKE_SOURCE_DIR}/packaging/config.default.toml
                ${CMAKE_CURRENT_BINARY_DIR}/config.default.toml
        DEPENDS ${CMAKE_SOURCE_DIR}/packaging/config.default.toml
        COMMENT "config.default.toml beside ${PROJECT_NAME}")
    add_custom_target(${PROJECT_NAME}_config_default ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/config.default.toml)
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_config_default)
endif()

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

# Windows SDK libraries needed at link time. Other hosts get graphics (SDL3,
# Metal), input and crypto through the xboxrecomp targets and need nothing
# extra here.
#
# The executable stays unstripped on every host: the fault handler names the
# generated function a crash happened in from the symbol table (dbghelp on
# Windows, dladdr elsewhere). macOS needs no -export_dynamic for that, only no
# strip -x / -Wl,-x.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
        winmm
        # Lets the fault handler resolve the host RIP back to the generated
        # function's symbol, i.e. to the guest function that faulted.
        dbghelp
    )
endif()

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()

# The lifter writes compares as `if ((a == b))`, which Clang (Apple clang and
# llvm-mingw alike) flags as -Wparentheses-equality once per site: tens of
# thousands of warnings a full build, burying the real ones. Generated code
# only.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set_property(SOURCE ${RECOMP_GEN_SOURCES} APPEND PROPERTY
        COMPILE_OPTIONS -Wno-parentheses-equality)
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )
