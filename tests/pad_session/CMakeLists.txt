cmake_minimum_required(VERSION 3.20)
project(pad_session_test C)
# A pad session started in the game (src/usb/pad_session.c,
# pad-recording-ui R2, R3, R7), driven as usb_gamepad.c drives it: one
# padrec_sync per port-0 poll, then padrec_press before the title reads the
# report and padrec_feed after. A record started at poll 1000 writes g0; its
# playback started at poll 5000 presses there; a stop writes one @end; a
# session is swapped in at the next poll, not before; Play again waits,
# ARMED, for the release; an ARMED record cancelled leaves no file; a file
# with no header is refused; an env key wins; the clock pick.
# padrec_stub.c beside this is the menu side's fake for the enhancement
# layer's tests, which build without a pad.
# test_threads: the pad, game and menu threads at once, and the record cap.
# -DPAD_SESSION_TSAN=ON builds all of it, the runtime too, under
# ThreadSanitizer (tests/posix_ctest.sh's pad_session_tsan).
set(CMAKE_C_STANDARD 11)
option(PAD_SESSION_TSAN "Build with ThreadSanitizer" OFF)
if(PAD_SESSION_TSAN)
    # The shared build (tests/CMakeLists.txt) has added the runtime already,
    # uninstrumented: TSan over half the program reports false races.
    if(XR_TEST_SHARED_RUNTIME)
        message(FATAL_ERROR "PAD_SESSION_TSAN needs its own runtime build: configure tests/pad_session on its own")
    endif()
    # A compiler that cannot link TSan reports n/a instead of failing.
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_c_source_compiles("int main(void) { return 0; }" XR_HAVE_TSAN)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT XR_HAVE_TSAN)
        message(FATAL_ERROR "pad_session_tsan: this compiler cannot build -fsanitize=thread")
    endif()
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
find_package(Threads REQUIRED)

add_executable(pad_session_test test_main.c)
target_include_directories(pad_session_test PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(pad_session_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME pad_session_test COMMAND pad_session_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_executable(pad_session_threads test_threads.c)
target_include_directories(pad_session_threads PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(pad_session_threads PRIVATE xboxrecomp Threads::Threads)
add_test(NAME pad_session_threads COMMAND pad_session_threads
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
