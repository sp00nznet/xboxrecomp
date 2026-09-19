# Cross-compiling the runtime libraries for Windows with MinGW-w64.
#
#     cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#     cmake --build build
#
# MSVC on Windows remains the reference environment; this only adds a host.
# Nothing here is read unless CMAKE_TOOLCHAIN_FILE names it, so a Visual
# Studio build is unaffected by the file existing.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# The triplet is overridable because distributions disagree about it:
# Debian and Arch ship x86_64-w64-mingw32-gcc, some others prefix differently.
if(NOT DEFINED MINGW_TRIPLET)
    set(MINGW_TRIPLET x86_64-w64-mingw32)
endif()

set(CMAKE_C_COMPILER   ${MINGW_TRIPLET}-gcc)
set(CMAKE_CXX_COMPILER ${MINGW_TRIPLET}-g++)
set(CMAKE_RC_COMPILER  ${MINGW_TRIPLET}-windres)

# Look for headers and libraries in the cross root, and for programs on the
# host -- otherwise CMake tries to run the target's tools. Overridable too:
# Debian and Arch put the root at /usr/<triplet>, Fedora one level deeper at
# /usr/<triplet>/sys-root/mingw.
if(NOT DEFINED MINGW_SYSROOT)
    set(MINGW_SYSROOT /usr/${MINGW_TRIPLET})
endif()
set(CMAKE_FIND_ROOT_PATH ${MINGW_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
