cmake_minimum_required(VERSION 3.20)
project(kernel_eeprom_settings_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_eeprom_settings_test test_main.c)
target_include_directories(kernel_eeprom_settings_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_eeprom_settings_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_eeprom_settings_test
    COMMAND kernel_eeprom_settings_test 0x00400000)
add_test(NAME kernel_eeprom_settings_widescreen
    COMMAND kernel_eeprom_settings_test 0x00410000)
set_tests_properties(kernel_eeprom_settings_widescreen PROPERTIES
    ENVIRONMENT "RECOMP_VIDEO_WIDESCREEN=1")
add_test(NAME kernel_eeprom_settings_480p
    COMMAND kernel_eeprom_settings_test 0x00480000)
set_tests_properties(kernel_eeprom_settings_480p PROPERTIES
    ENVIRONMENT "RECOMP_VIDEO_480P=1")
add_test(NAME kernel_eeprom_settings_widescreen_480p
    COMMAND kernel_eeprom_settings_test 0x00490000)
set_tests_properties(kernel_eeprom_settings_widescreen_480p PROPERTIES
    ENVIRONMENT "RECOMP_VIDEO_WIDESCREEN=1;RECOMP_VIDEO_480P=1")
add_test(NAME kernel_eeprom_settings_zero_is_off
    COMMAND kernel_eeprom_settings_test 0x00400000)
set_tests_properties(kernel_eeprom_settings_zero_is_off PROPERTIES
    ENVIRONMENT "RECOMP_VIDEO_WIDESCREEN=0;RECOMP_VIDEO_480P=0")
