# saves_keys - the console keys stay zero (pad-recording-ui R6, save
# safeguard 1). The in-game re-sign (src/enhance/saves_sign.c) and the
# CLI's save import sign under the zero key; that is right only while the
# build's XboxHDKey and XboxSignatureKey are zero, so this links the real
# arrays and xbox_saves_keys_id (src/kernel/kernel_keys.c) and checks them.
# crypto_stubs.c gives kernel_crypto.c its two host calls without the
# runtime; tests/saves_zip uses it too. Pure C, no device:
#   cmake -S tests/saves_keys -B build/saves_keys
#   cmake --build build/saves_keys && ctest --test-dir build/saves_keys
cmake_minimum_required(VERSION 3.20)
project(saves_keys_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(saves_keys_test
    test_main.c
    crypto_stubs.c
    ${XR_SRC}/kernel/kernel_keys.c
    ${XR_SRC}/kernel/kernel_crypto.c)
target_include_directories(saves_keys_test PRIVATE ${XR_SRC} ${XR_SRC}/kernel)
if(NOT MSVC)
    target_compile_options(saves_keys_test PRIVATE -Wall -Wextra -Wno-comment)
endif()
if(WIN32)
    target_link_libraries(saves_keys_test PRIVATE bcrypt)
endif()
enable_testing()
add_test(NAME saves_keys COMMAND saves_keys_test)
