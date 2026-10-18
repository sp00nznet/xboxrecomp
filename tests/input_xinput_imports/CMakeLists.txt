# input_xinput_imports - the XInput DLL a Windows exe linked against
# xbox_input imports. mingw-w64's plain libxinput.a names XINPUT1_3.dll, the
# DirectX redistributable that stock Windows 10/11 lacks, so such an exe
# fails to start there with 0xC0000135 while Proton (which ships the DLL)
# runs it fine. The probe links xbox_input exactly as a game does and the
# check reads its import table on the build host, no emulator needed:
#   cmake --build <build> --target input_xinput_probe && ctest -R input_xinput_imports
# Windows builds with an objdump only (llvm-mingw has one; MSVC does not).

add_executable(input_xinput_probe probe.c)
target_link_libraries(input_xinput_probe PRIVATE xbox_input)
enable_testing()
add_test(NAME input_xinput_imports
         COMMAND ${CMAKE_COMMAND}
                 -DOBJDUMP=${CMAKE_OBJDUMP}
                 -DEXE=$<TARGET_FILE:input_xinput_probe>
                 -P ${CMAKE_CURRENT_LIST_DIR}/check_imports.cmake)
