/**
 * D3DCompile and D3DCreateBlob for the Linux build of librashader's D3D11
 * runtime (build-linux.sh), over vkd3d-shader.
 *
 * On Windows those two come from d3dcompiler_47.dll. The Linux build runs the
 * same D3D11 runtime on dxvk-native, where the windows crate declares the
 * functions but links nothing, so this file is linked into librashader.so
 * to define them. vkd3d-shader is the library the game's own D3D11 backend
 * compiles with (src/d3d/d3d11_hlsl_vkd3d.c) and the one Wine's
 * d3dcompiler_47 wraps under Proton, so a preset compiles to the same DXBC
 * natively as under Proton. The options are vkd3d-utils' D3DCompile2's.
 *
 * Only what librashader calls is here: no include handler (SPIR-V Cross
 * writes whole sources) and an ID3DBlob with IUnknown and the two buffer
 * methods. The COM layout is the C one dxvk-native uses; on x86-64 Linux
 * the "system" ABI the windows crate calls through is the C ABI.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <vkd3d_shader.h>

typedef int32_t HRESULT;
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GUID;

#define S_OK            ((HRESULT)0)
#define E_NOINTERFACE   ((HRESULT)0x80004002u)
#define E_POINTER       ((HRESULT)0x80004003u)
#define E_FAIL          ((HRESULT)0x80004005u)
#define E_OUTOFMEMORY   ((HRESULT)0x8007000Eu)
#define E_INVALIDARG    ((HRESULT)0x80070057u)

/* D3DCOMPILE_* bits that vkd3d has an option for (d3dcompiler.h). */
#define PACK_MATRIX_ROW_MAJOR      (1u << 3)
#define PACK_MATRIX_COLUMN_MAJOR   (1u << 4)
#define BACKWARDS_COMPATIBILITY    (1u << 12)

typedef struct { const char *name, *definition; } D3D_SHADER_MACRO;

typedef struct blob blob;
struct blob_vtbl {
    HRESULT (*QueryInterface)(blob *, const GUID *, void **);
    uint32_t (*AddRef)(blob *);
    uint32_t (*Release)(blob *);
    void *(*GetBufferPointer)(blob *);
    size_t (*GetBufferSize)(blob *);
};
struct blob {
    const struct blob_vtbl *lpVtbl;
    uint32_t refs;
    size_t size;
    unsigned char data[];
};

/* IID_IUnknown {00000000-0000-0000-C000-000000000046} and IID_ID3D10Blob
 * {8BA5FB08-5195-40e2-AC58-0D989C3A0102} (ID3DBlob is ID3D10Blob). */
static const GUID iid_unknown = {0, 0, 0, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID iid_blob = {0x8ba5fb08, 0x5195, 0x40e2,
                              {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};

static uint32_t blob_addref(blob *b)
{
    return __atomic_add_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);
}

static uint32_t blob_release(blob *b)
{
    uint32_t n = __atomic_sub_fetch(&b->refs, 1, __ATOMIC_SEQ_CST);
    if (!n)
        free(b);
    return n;
}

static HRESULT blob_query(blob *b, const GUID *iid, void **out)
{
    if (!out)
        return E_POINTER;
    if (iid && (!memcmp(iid, &iid_unknown, sizeof *iid)
                || !memcmp(iid, &iid_blob, sizeof *iid))) {
        blob_addref(b);
        *out = b;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static void *blob_ptr(blob *b) { return b->data; }
static size_t blob_size(blob *b) { return b->size; }

static const struct blob_vtbl blob_vtbl = {
    blob_query, blob_addref, blob_release, blob_ptr, blob_size,
};

static blob *blob_new(const void *data, size_t size)
{
    blob *b = malloc(sizeof *b + (size ? size : 1));
    if (!b)
        return NULL;
    b->lpVtbl = &blob_vtbl;
    b->refs = 1;
    b->size = size;
    if (data && size)
        memcpy(b->data, data, size);
    else
        memset(b->data, 0, size);
    return b;
}

__attribute__((visibility("default")))
HRESULT D3DCreateBlob(size_t size, blob **out)
{
    if (!out)
        return E_POINTER;
    *out = blob_new(NULL, size);
    return *out ? S_OK : E_OUTOFMEMORY;
}

__attribute__((visibility("default")))
HRESULT D3DCompile(const void *src, size_t len, const char *name,
                   const D3D_SHADER_MACRO *defines, void *include,
                   const char *entry, const char *target, uint32_t flags1,
                   uint32_t flags2, blob **code, blob **errors)
{
    struct vkd3d_shader_compile_option opt[3];
    struct vkd3d_shader_preprocess_info pp;
    struct vkd3d_shader_hlsl_source_info hlsl;
    struct vkd3d_shader_compile_info info;
    struct vkd3d_shader_macro *macros = NULL;
    struct vkd3d_shader_code out;
    char *log = NULL;
    unsigned n = 0, nmacros = 0;
    int rc;

    (void)flags2;   /* effect flags: no effects are compiled */
    if (errors)
        *errors = NULL;
    if (!code || !src || !target)
        return E_INVALIDARG;
    *code = NULL;
    if (include)    /* librashader never passes one */
        return E_INVALIDARG;

    opt[n].name = VKD3D_SHADER_COMPILE_OPTION_API_VERSION;
    opt[n++].value = VKD3D_SHADER_API_VERSION_1_14;
    if (flags1 & PACK_MATRIX_ROW_MAJOR) {
        opt[n].name = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER;
        opt[n++].value = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ROW_MAJOR;
    } else if (flags1 & PACK_MATRIX_COLUMN_MAJOR) {
        opt[n].name = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_ORDER;
        opt[n++].value = VKD3D_SHADER_COMPILE_OPTION_PACK_MATRIX_COLUMN_MAJOR;
    }
    if (flags1 & BACKWARDS_COMPATIBILITY) {
        opt[n].name = VKD3D_SHADER_COMPILE_OPTION_BACKWARD_COMPATIBILITY;
        opt[n++].value = VKD3D_SHADER_COMPILE_OPTION_BACKCOMPAT_MAP_SEMANTIC_NAMES;
    }

    memset(&hlsl, 0, sizeof hlsl);
    hlsl.type = VKD3D_SHADER_STRUCTURE_TYPE_HLSL_SOURCE_INFO;
    hlsl.entry_point = entry;
    hlsl.profile = target;

    if (defines) {
        while (defines[nmacros].name)
            nmacros++;
    }
    if (nmacros) {
        macros = calloc(nmacros, sizeof *macros);
        if (!macros)
            return E_OUTOFMEMORY;
        for (unsigned i = 0; i < nmacros; i++) {
            macros[i].name = defines[i].name;
            macros[i].value = defines[i].definition ? defines[i].definition : "";
        }
        memset(&pp, 0, sizeof pp);
        pp.type = VKD3D_SHADER_STRUCTURE_TYPE_PREPROCESS_INFO;
        pp.macros = macros;
        pp.macro_count = nmacros;
        hlsl.next = &pp;
    }

    memset(&info, 0, sizeof info);
    info.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.next = &hlsl;
    info.source.code = src;
    info.source.size = len;
    info.source_type = VKD3D_SHADER_SOURCE_HLSL;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.options = opt;
    info.option_count = n;
    info.log_level = VKD3D_SHADER_LOG_INFO;
    info.source_name = name;

    memset(&out, 0, sizeof out);
    rc = vkd3d_shader_compile(&info, &out, &log);
    free(macros);
    if (log) {
        size_t l = strlen(log);
        if (errors && l)
            *errors = blob_new(log, l + 1);
        vkd3d_shader_free_messages(log);
    }
    if (rc < 0) {
        vkd3d_shader_free_shader_code(&out);
        return rc == VKD3D_ERROR_OUT_OF_MEMORY ? E_OUTOFMEMORY : E_FAIL;
    }
    *code = blob_new(out.code, out.size);
    vkd3d_shader_free_shader_code(&out);
    return *code ? S_OK : E_OUTOFMEMORY;
}
