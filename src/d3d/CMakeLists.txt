# D3D8 → D3D11 Compatibility Layer
# Implements Xbox D3D8 interface backed by Direct3D 11

add_library(xbox_d3d8 STATIC
    d3d8_device.c
    d3d8_resources.c
    d3d8_shaders.c
    d3d8_states.c
    d3d8_combiners.c
)

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(xbox_d3d8 PUBLIC d3d11 dxgi dxguid d3dcompiler)

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -O2)
endif()
