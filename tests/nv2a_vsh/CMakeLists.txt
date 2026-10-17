cmake_minimum_required(VERSION 3.20)
project(nv2a_vsh_regression C)
enable_testing()

set(KERNEL ../../src/kernel)
set(D3D ../../src/d3d)

# Hand-encoded XDK-shaped programs against the interpreter.
add_executable(nv2a-vsh-test test_main.c ${KERNEL}/nv2a_vsh_cpu.c)
# Upstream's epilogue regression, on the stateless API.
add_executable(nv2a-vsh-epilogue nv2a_vsh_epilogue_test.c ${KERNEL}/nv2a_vsh_cpu.c)
# Interpreter and parser against their pre-nv2a_vsh_fields.h versions.
add_executable(nv2a-vsh-fuzz nv2a_vsh_fuzz.c
    ${KERNEL}/nv2a_vsh_cpu.c ${D3D}/d3d8_vsh_parse.c
    oracle/nv2a_vsh_cpu_frozen.c oracle/d3d8_vsh_parse_frozen.c)

foreach(t nv2a-vsh-test nv2a-vsh-epilogue nv2a-vsh-fuzz)
    target_include_directories(${t} PRIVATE ${KERNEL} ${D3D})
    if(NOT WIN32)
        target_link_libraries(${t} PRIVATE m)
    endif()
endforeach()

add_test(NAME nv2a-vsh-programs COMMAND nv2a-vsh-test)
add_test(NAME nv2a-vsh-epilogue COMMAND nv2a-vsh-epilogue)
add_test(NAME nv2a-vsh-fuzz COMMAND nv2a-vsh-fuzz)
