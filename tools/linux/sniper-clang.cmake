# Cross-build for Linux x86-64 on the Mac with a native clang and lld against
# the sniper sysroot (tools/linux/sysroot.sh), the design D4 alternative to
# building under amd64 emulation. The binaries link against sniper's glibc
# 2.31 and libgcc, exactly as the SDK image's gcc would.
#
#   cmake -S . -B build-linux -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=tools/linux/sniper-clang.cmake \
#     -DSNIPER_SYSROOT=<sysroot> -DSNIPER_LLVM=<llvm bin dir>
#
# SNIPER_LLVM is any clang with the X86 target and ld.lld beside it (the
# pinned llvm-mingw has both).
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(SNIPER_SYSROOT "$ENV{SNIPER_SYSROOT}" CACHE PATH "sniper sysroot")
set(SNIPER_LLVM "$ENV{SNIPER_LLVM}" CACHE PATH "Directory holding clang and ld.lld")
if(NOT SNIPER_SYSROOT OR NOT EXISTS "${SNIPER_SYSROOT}/usr/include/stdio.h")
    message(FATAL_ERROR "SNIPER_SYSROOT must point at the exported sniper sysroot")
endif()
# try_compile projects re-read this file; pass both down to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SNIPER_SYSROOT SNIPER_LLVM)

set(_triple x86_64-linux-gnu)
if(SNIPER_LLVM)
    set(CMAKE_C_COMPILER "${SNIPER_LLVM}/clang")
    set(CMAKE_CXX_COMPILER "${SNIPER_LLVM}/clang++")
    set(CMAKE_AR "${SNIPER_LLVM}/llvm-ar" CACHE FILEPATH "")
    set(CMAKE_RANLIB "${SNIPER_LLVM}/llvm-ranlib" CACHE FILEPATH "")
else()
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
endif()
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_SYSROOT "${SNIPER_SYSROOT}")

# libgcc and the crt objects come from the sysroot's gcc, as with the SDK.
set(_flags "--gcc-toolchain=${SNIPER_SYSROOT}/usr")
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld ${_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld ${_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld ${_flags}")

set(CMAKE_FIND_ROOT_PATH "${SNIPER_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)

# pkg-config (epoxy) reads the sysroot's .pc files, never the Mac's.
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SNIPER_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${SNIPER_SYSROOT}/usr/lib/x86_64-linux-gnu/pkgconfig:${SNIPER_SYSROOT}/usr/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
