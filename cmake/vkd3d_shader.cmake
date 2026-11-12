# vkd3d-shader for the XBOXRECOMP_DXVK_NATIVE build: the HLSL compiler
# d3d11_hlsl_vkd3d.c calls. The pin below is the one source of the version:
# tools/linux/vkd3d.sh reads it, downloads that tarball, checks its sha256
# and builds the library unmodified in the sniper SDK image (on any host
# with Docker), into a directory this build takes as
# XBOXRECOMP_VKD3D_SHADER_DIR. 2.1 is GE-Proton11-7's (its d3dcompiler_47
# writes "vkd3d-shader 2.1" in every DXBC), so Proton and the native build
# compile the same bytes.
#
#   xboxrecomp_vkd3d_shader(<target>)
#
# Adds the include directory and the library to <target>. With
# XBOXRECOMP_VKD3D_SHADER_DIR unset, the sysroot's libvkd3d-shader is used
# through pkg-config (a dev build; the sniper SDK's is a Debian 2.1 build),
# with a note: a package ships the pinned build, whose source the package
# names.

set(XBOXRECOMP_VKD3D_SHADER_VERSION 2.1)
set(XBOXRECOMP_VKD3D_SHADER_URL
    "https://dl.winehq.org/vkd3d/source/vkd3d-${XBOXRECOMP_VKD3D_SHADER_VERSION}.tar.xz")
set(XBOXRECOMP_VKD3D_SHADER_SHA256
    7510146aff2adfb4ae07ab890701a607e5ff7c66e57100cfc9f630ec92eeda6a)

set(XBOXRECOMP_VKD3D_SHADER_DIR "" CACHE PATH
    "libvkd3d-shader built by tools/linux/vkd3d.sh (default: the sysroot's, via pkg-config)")

function(xboxrecomp_vkd3d_shader target)
    set(dir "${XBOXRECOMP_VKD3D_SHADER_DIR}")
    if(dir)
        set(want "vkd3d ${XBOXRECOMP_VKD3D_SHADER_VERSION}\nsha256 ${XBOXRECOMP_VKD3D_SHADER_SHA256}\n")
        set(have "")
        if(EXISTS "${dir}/SOURCE")
            file(READ "${dir}/SOURCE" have)
        endif()
        if(NOT have STREQUAL want)
            message(FATAL_ERROR "vkd3d-shader: ${dir}/SOURCE does not name the pin "
                "(vkd3d ${XBOXRECOMP_VKD3D_SHADER_VERSION}, sha256 "
                "${XBOXRECOMP_VKD3D_SHADER_SHA256}); rebuild it with tools/linux/vkd3d.sh")
        endif()
        if(NOT EXISTS "${dir}/lib/libvkd3d-shader.so.1"
           OR NOT EXISTS "${dir}/include/vkd3d/vkd3d_shader.h")
            message(FATAL_ERROR "vkd3d-shader: ${dir} has no lib/libvkd3d-shader.so.1 "
                "or include/vkd3d/vkd3d_shader.h")
        endif()
        target_include_directories(${target} PRIVATE "${dir}/include/vkd3d")
        target_link_libraries(${target} PUBLIC "${dir}/lib/libvkd3d-shader.so.1")
        message(STATUS "vkd3d-shader: ${XBOXRECOMP_VKD3D_SHADER_VERSION}, pinned build (${dir})")
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(VKD3D_SHADER REQUIRED libvkd3d-shader)
        target_include_directories(${target} PRIVATE ${VKD3D_SHADER_INCLUDE_DIRS})
        target_link_libraries(${target} PUBLIC ${VKD3D_SHADER_LINK_LIBRARIES})
        message(STATUS "vkd3d-shader: ${VKD3D_SHADER_VERSION} from pkg-config; a "
            "package ships the pinned ${XBOXRECOMP_VKD3D_SHADER_VERSION} "
            "(tools/linux/vkd3d.sh, XBOXRECOMP_VKD3D_SHADER_DIR)")
    endif()
endfunction()
