# template_main - the new-game template's src/main.c compiles and links against
# the toolkit, on every host the toolkit builds on, and starts. xbr new copies
# that file into every new game, and nothing else built it: twice the runtime
# moved on (the APU hook, the recomp_dispatch_init declaration) and the
# template only found out in the next game. Cross-platform; the Proton
# standalone list runs it too. Standalone:
#   cmake -S tests/template_main -B build/template_main
#   cmake --build build/template_main && ctest --test-dir build/template_main
#
# No test boots: the memory layout wants a real XBE, and the boot itself is
# what a real game's run checks. stubs.c stands in for the generated code.
# Run it twice, with -DXBOXRECOMP_ENHANCE=ON and without: the template's
# enhancements hook builds and runs either way.
cmake_minimum_required(VERSION 3.20)
project(template_main_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

# As in the template's CMakeLists.txt: OpenSSL::Crypto in scope off Windows.
if(NOT WIN32)
    find_package(OpenSSL REQUIRED)
endif()
add_subdirectory(../.. xboxrecomp)

set(TEMPLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../templates/new-game)
add_executable(template_main_test WIN32 ${TEMPLATE_DIR}/src/main.c stubs.c)
target_link_libraries(template_main_test PRIVATE xboxrecomp)
target_include_directories(template_main_test PRIVATE
    ${TEMPLATE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
if(WIN32)
    target_link_libraries(template_main_test PRIVATE
        d3d11 dxgi dxguid $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput> winmm dbghelp)
endif()

enable_testing()
# Run through a script: each test checks the exit status and where the output
# went. Under a cross build the script runs the exe through the emulator
# (tests/proton_run.sh), which runs it from the exe's own directory.
# ENHANCE says whether the layer is in (-DXBOXRECOMP_ENHANCE=ON): its
# [ENHANCE] line must then be in the output, and must not be otherwise.
if(XBOXRECOMP_ENHANCE)
    set(want_enhance 1)
else()
    set(want_enhance 0)
endif()
foreach(t no_xbe stdio_log game_files)
    add_test(NAME template_main_${t}
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:template_main_test>
            "-DEMU=${CMAKE_CROSSCOMPILING_EMULATOR}"
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work_${t}
            -DMODE=${t}
            -DENHANCE=${want_enhance}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run.cmake)
endforeach()
# The template's own lines, read as text: the defaults and the example line
# the CLI patches. No build or host needed.
add_test(NAME template_main_defaults
    COMMAND ${CMAKE_COMMAND} -DMAIN=${TEMPLATE_DIR}/src/main.c
        -P ${CMAKE_CURRENT_SOURCE_DIR}/defaults.cmake)
