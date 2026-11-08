# saves_zip - the menu's saves zip (src/enhance/saves_zip.c, saves_store.c;
# openspec change overlay-saves, D2, D3, D5, D6): the writer read back,
# every refusal of the reader and the saves check (caps, zip-slip, '..',
# backslashes, drive letters, device names, case duplicates, links,
# checksums, header mismatches, compressed and encrypted entries, overlaps,
# the manifest, another title, entries outside the title, the keys), the
# SaveMeta.xbx name, and the store on a temp folder: the listing, an export
# and its import onto other saves, a refused import and a busy one leaving
# the saves as they were; the bug-report bundle (pad-recording-ui R6): its
# manifest, its pad/ names, an export of the saves read back and imported,
# and each refusal (chain entry with no file, stray or nested pad/ file,
# over the cap, another title, a saves zip). Pure C, no device:
#   cmake -S tests/saves_zip -B build/saves_zip
#   cmake --build build/saves_zip && ctest --test-dir build/saves_zip
cmake_minimum_required(VERSION 3.20)
project(saves_zip_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(saves_zip_test
    test_main.c
    ${XR_SRC}/enhance/saves_zip.c
    ${XR_SRC}/enhance/saves_store.c
    ${XR_SRC}/enhance/file_browser.c
    ${XR_SRC}/enhance/saves_sign.c
    ${XR_SRC}/kernel/kernel_crypto.c
    ${CMAKE_CURRENT_LIST_DIR}/../saves_keys/crypto_stubs.c)
target_include_directories(saves_zip_test PRIVATE ${XR_SRC}/enhance ${XR_SRC} ${XR_SRC}/kernel)
if(NOT MSVC)
    target_compile_options(saves_zip_test PRIVATE -Wall -Wextra -Wno-comment)
endif()
if(WIN32)
    target_link_libraries(saves_zip_test PRIVATE bcrypt)
endif()
enable_testing()
add_test(NAME saves_zip COMMAND saves_zip_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
