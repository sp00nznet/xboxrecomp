cmake_minimum_required(VERSION 3.20)
project(apu_vp_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
# test_main.c compiles apu_vp.c itself, to reach its static helpers, and
# stands in for the little it uses from the rest of the APU.
add_executable(apu_vp_test test_main.c)
target_include_directories(apu_vp_test PRIVATE ../../src ../../src/apu)
target_compile_definitions(apu_vp_test PRIVATE _USE_MATH_DEFINES)
# The platform layer gives recomp_env and, off Windows, the Win32 shims.
target_link_libraries(apu_vp_test PRIVATE platform)
enable_testing()
add_test(NAME apu_vp COMMAND apu_vp_test)
