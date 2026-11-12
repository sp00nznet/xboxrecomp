/**
 * d3d11_hlsl.h over vkd3d-shader (libvkd3d-shader, LGPL-2.1+, linked as
 * its own shared library). Includes no Windows or DXVK header: see
 * d3d11_hlsl.h for why.
 *
 * The options are the ones vkd3d-utils' D3DCompile2 sets (Wine's
 * d3dcompiler_47 under Proton is that code), so the DXBC matches what the
 * Proton golden ran: the API version this file was written against, and the
 * D3DCOMPILE flags that have a vkd3d option. The backend passes only
 * D3DCOMPILE_OPTIMIZATION_LEVEL3, which vkd3d has no option for; Proton's
 * log says "Ignoring flags 0x8000" for it.
 */
#include "d3d11_hlsl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d_shader.h>

/* D3DCOMPILE_* (d3dcompiler.h), spelled out: that header is not included
 * here. */
#define HLSL_PACK_MATRIX_ROW_MAJOR      (1u << 3)
#define HLSL_PACK_MATRIX_COLUMN_MAJOR   (1u << 4)
#define HLSL_BACKWARDS_COMPATIBILITY    (1u << 12)

/* fxc's spelling of infinity, which the vertex emitter writes for LOG of 0
 * (d3d8_vsh_hlsl.c) and vkd3d-shader's parser rejects as a syntax error. The
 * emitter's text stays fxc's, so the Windows DXBC and the hashed text
 * (tests/d3d8_hlsl_split) do not move; it is respelled here, for this
 * compiler only, as the same bits through asfloat. */
static const char k_fxc_inf[] = "1.#INF";
static const char k_hlsl_inf[] = "asfloat(0x7f800000)";

/* A malloc'd copy of src (len bytes) with every k_fxc_inf respelled, and
 * its length in *out_len; NULL when src has none, or out of memory. */
static char *respell_inf(const char *src, size_t len, size_t *out_len)
{
    const size_t fl = sizeof k_fxc_inf - 1, hl = sizeof k_hlsl_inf - 1;
    size_t i, n = 0, o = 0;
    char *dst;

    for (i = 0; i + fl <= len; i++)
        if (!memcmp(src + i, k_fxc_inf, fl)) {
            n++;
            i += fl - 1;
        }
    if (!n)
        return NULL;
    dst = malloc(len + n * (hl - fl) + 1);
    if (!dst)
        return NULL;
    for (i = 0; i < len;) {
        if (i + fl <= len && !memcmp(src + i, k_fxc_inf, fl)) {
            memcpy(dst + o, k_hlsl_inf, hl);
            o += hl;
            i += fl;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
    *out_len = o;
    return dst;
}

const char *d3d11_hlsl_version(void)
{
    static char v[64];
    if (!v[0]) {
        unsigned major = 0, minor = 0;
        const char *s = vkd3d_shader_get_version(&major, &minor);
        /* The library's own string names its build ("vkd3d-shader 1.14");
         * the numbers are kept in case a build's string says less. */
        snprintf(v, sizeof v, "%s (%u.%u)", s ? s : "vkd3d-shader", major, minor);
    }
    return v;
}

unsigned d3d11_hlsl_flags(unsigned flags1)
{
    return flags1 & (HLSL_PACK_MATRIX_ROW_MAJOR | HLSL_PACK_MATRIX_COLUMN_MAJOR
                     | HLSL_BACKWARDS_COMPATIBILITY);
}

int d3d11_hlsl_compile(const void *src, size_t len, const char *name, const char *entry,
                       const char *target, unsigned flags1, unsigned flags2,
                       void **code, size_t *code_len, char **messages)
{
    struct vkd3d_shader_compile_option opt[4];
    struct vkd3d_shader_hlsl_source_info hlsl;
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_code out;
    char *log = NULL, *respelled;
    size_t respelled_len = 0;
    unsigned n = 0;
    int rc;

    (void)flags2;   /* D3DCompile's effect flags: no effects are compiled */
    *code = NULL;
    *code_len = 0;
    if (messages)
        *messages = NULL;

    opt[n].name = VKD3D_SHADER_COMPILE_OPTION_API_VERSION;
    opt[n++].value = VKD3D_SHADER_API_VERSION_1_14;
    if (flags1 & HLSL_PACK_MATRIX_ROW_MAJOR) {
        opt[n].name = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER;
        opt[n++].value = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR;
    } else if (flags1 & HLSL_PACK_MATRIX_COLUMN_MAJOR) {
        opt[n].name = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER;
        opt[n++].value = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR;
    }
    if (flags1 & HLSL_BACKWARDS_COMPATIBILITY) {
        opt[n].name = VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY;
        opt[n++].value = VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES;
    }

    memset(&hlsl, 0, sizeof hlsl);
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.entry_point = entry;
    hlsl.profile = target;

    memset(&info, 0, sizeof info);
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    respelled = respell_inf(src, len, &respelled_len);
    info.source.code = respelled ? respelled : src;
    info.source.size = respelled ? respelled_len : len;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.options = opt;
    info.option_count = n;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = name;

    memset(&out, 0, sizeof out);
    rc = vkd3d_shader_compile(&info, &out, &log);
    free(respelled);
    if (log) {
        if (messages && *log)
            *messages = strdup(log);
        vkd3d_shader_free_messages(log);
    }
    if (rc < 0) {
        vkd3d_shader_free_shader_code(&out);
        return rc;
    }
    /* Copied into malloc'd memory, so one free() fits every caller. */
    *code = malloc(out.size ? out.size : 1);
    if (!*code) {
        vkd3d_shader_free_shader_code(&out);
        return VKD3D_ERROR_OUT_OF_MEMORY;
    }
    memcpy(*code, out.code, out.size);
    *code_len = out.size;
    vkd3d_shader_free_shader_code(&out);
    return 0;
}

void d3d11_hlsl_free(void *p)
{
    free(p);
}
