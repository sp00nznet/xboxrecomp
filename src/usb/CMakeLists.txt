# USB: MCPX OHCI host controllers, so a title's own XAPI can find a pad

add_library(xbox_usb STATIC
    ohci.c
    usb_gamepad.c
)

target_include_directories(xbox_usb PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# xbox_devbus for xbox_ContiguousIsPhysical: descriptors arrive as bus addresses.
target_link_libraries(xbox_usb PUBLIC platform xbox_input xbox_devbus)

if(MSVC)
    target_compile_options(xbox_usb PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_usb PRIVATE -Wall -Wextra -O2)
endif()
