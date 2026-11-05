cmake_minimum_required(VERSION 3.20)
project(pad_script_test C)
# The pad script's grammar and the recorder (pad_script.c) know a 20-byte
# report and text, nothing of USB, so the test builds them alone.
set(CMAKE_C_STANDARD 11)
add_executable(pad_script_test test_main.c ../../src/usb/pad_script.c)
target_include_directories(pad_script_test PRIVATE ../../src/usb)
enable_testing()
add_test(NAME pad_script_test COMMAND pad_script_test)
