cmake_minimum_required(VERSION 3.20)
project(usb_pad_game_test C)
# The pad script and the pad record for a game that answers XInputGetState
# itself (usb_gamepad.h, usb_gamepad_game_*): the script presses on the
# game's state, the record writes it, and the record replays as a script to
# the same states. No guest memory and no USB transfer: the functions are
# called directly, so this runs on every host.
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()

add_executable(usb_pad_game_test test_main.c)
target_include_directories(usb_pad_game_test PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(usb_pad_game_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME usb_pad_game_test COMMAND usb_pad_game_test)

# test_session: a pad session started in the game (pad-recording-ui) on the
# same path, with no env key: its own binary, since the env is read once.
add_executable(usb_pad_game_session test_session.c)
target_include_directories(usb_pad_game_session PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(usb_pad_game_session PRIVATE xboxrecomp)
add_test(NAME usb_pad_game_session COMMAND usb_pad_game_session
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# test_env_chain: RECOMP_PAD_SCRIPT=@a.pad;@b.pad, a chain the session code
# plays, whose @open anchors see the game's file opens.
add_executable(usb_pad_game_env_chain test_env_chain.c)
target_include_directories(usb_pad_game_env_chain PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(usb_pad_game_env_chain PRIVATE xboxrecomp)
add_test(NAME usb_pad_game_env_chain COMMAND usb_pad_game_env_chain
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
