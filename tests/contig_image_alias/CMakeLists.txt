cmake_minimum_required(VERSION 3.20)
project(contig_image_alias_test C)
# No contiguous block shares a physical number with a named image page.
# Cross-platform: the arena and the resolvers are the same code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(contig_image_alias_test test_main.c)
target_include_directories(contig_image_alias_test PRIVATE ../../src ../../src/apu ../../src/kernel)
target_link_libraries(contig_image_alias_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the image-name record is never cleared and the
# arena's switch is read once.
add_test(NAME contig_image_alias_named COMMAND contig_image_alias_test named ${XBE})
add_test(NAME contig_image_alias_unnamed COMMAND contig_image_alias_test unnamed ${XBE})
add_test(NAME contig_image_alias_late COMMAND contig_image_alias_test late ${XBE})
add_test(NAME contig_image_alias_fill_named COMMAND contig_image_alias_test fill-named ${XBE})
