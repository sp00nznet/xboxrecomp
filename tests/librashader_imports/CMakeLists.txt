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

enable_testing()
if(WIN32)
    _xboxrecomp_librashader_platform(_lrs_plat)
else()
    set(_lrs_plat x86_64-windows)
endif()
if(_lrs_plat STREQUAL "x86_64-windows")
    _xboxrecomp_librashader_fetch_plat(${_lrs_plat} _lrs_lib _lrs_notices)
    add_test(NAME librashader_imports
             COMMAND ${CMAKE_COMMAND}
                     -DOBJDUMP=${CMAKE_OBJDUMP}
                     -DDLL=${_lrs_lib}
                     -P ${CMAKE_CURRENT_LIST_DIR}/check_imports.cmake)
    # An empty DLL: the configure could not download the zip (it warned).
    set_tests_properties(librashader_imports PROPERTIES
                         SKIP_REGULAR_EXPRESSION "librashader_imports: skipped")
endif()
