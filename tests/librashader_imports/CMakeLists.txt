# librashader_imports - the dlls the staged x86-64 Windows librashader.dll
# imports. The upstream prebuilt imports D3DX9_43.dll (DirectX June 2010
# runtime) and the VC++ runtime, neither on a stock Windows 11, so
# LoadLibrary failed with error 126 there while Proton (which ships both)
# ran it. cmake/librashader.cmake now pins our D3D11-only llvm-mingw build;
# this reads its import table on the build host, no emulator needed:
#   ctest -R librashader_imports
# Windows builds read the dll they stage, with an objdump only (llvm-mingw
# has one; MSVC does not). Other hosts read the same pinned zip when
# configured with -DXBOXRECOMP_LIBRASHADER_TEST_WIN64=ON (tests/posix_ctest.sh
# does): its objdump reads PE files too (llvm-objdump on macOS, binutils on
# Linux). The test skips when the zip could not be downloaded.
#
# librashader_linux_needs does the same for the x86-64 Linux librashader.so
# (third_party/librashader/build-linux.sh), which the dxvk-native build
# stages and the bundle's launch.sh runs in the Steam Linux Runtime: its
# NEEDED libraries are glibc's, libgcc_s and the bundled libvkd3d-shader,
# and no symbol asks for a glibc newer than sniper's 2.31 or for libstdc++
# (linked statically; the bundle ships none). A dxvk-native build reads the
# library it stages and fails without it, since its bundle would then ship
# no librashader.so; any other build reads it with the option above and
# skips when the zip could not be downloaded.

enable_testing()
if(WIN32 OR XBOXRECOMP_DXVK_NATIVE)
    _xboxrecomp_librashader_platform(_lrs_plat)
else()
    set(_lrs_plat "")
endif()
if(_lrs_plat STREQUAL "x86_64-windows" OR XBOXRECOMP_LIBRASHADER_TEST_WIN64)
    _xboxrecomp_librashader_fetch_plat(x86_64-windows _lrs_lib _lrs_notices)
    add_test(NAME librashader_imports
             COMMAND ${CMAKE_COMMAND}
                     -DOBJDUMP=${CMAKE_OBJDUMP}
                     -DDLL=${_lrs_lib}
                     -P ${CMAKE_CURRENT_LIST_DIR}/check_imports.cmake)
    # An empty DLL: the configure could not download the zip (it warned).
    set_tests_properties(librashader_imports PROPERTIES
                         SKIP_REGULAR_EXPRESSION "librashader_imports: skipped")
endif()
if(_lrs_plat STREQUAL "x86_64-linux" OR XBOXRECOMP_LIBRASHADER_TEST_WIN64)
    _xboxrecomp_librashader_fetch_plat(x86_64-linux _lrs_so _lrs_so_notices)
    add_test(NAME librashader_linux_needs
             COMMAND ${CMAKE_COMMAND}
                     -DOBJDUMP=${CMAKE_OBJDUMP}
                     -DSO=${_lrs_so}
                     -DREQUIRED=$<STREQUAL:${_lrs_plat},x86_64-linux>
                     -P ${CMAKE_CURRENT_LIST_DIR}/check_linux.cmake)
    set_tests_properties(librashader_linux_needs PROPERTIES
                         SKIP_REGULAR_EXPRESSION "librashader_linux_needs: skipped")
endif()
