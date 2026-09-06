/**
 * Xbox Direct3D 8 Compatibility Layer - Type Definitions
 *
 * Defines the Xbox D3D8 types, enums, and COM interface structures
 * used by the statically-linked RenderWare Xbox driver code.
 *
 * Xbox D3D8 differs from PC D3D8 in several ways:
 * - Push buffer (command buffer) based rendering
 * - Tiled/swizzled texture formats
 * - Hardware-specific render states
 * - Unified 64MB memory model (textures/VBs in main RAM)
 * - No CAPS querying (known fixed hardware)
 *
 * This header provides the ABI-compatible types so that translated
 * game/RW code can compile against our D3D11-backed implementation.
 */

#ifndef BURNOUT3_D3D8_XBOX_H
#define BURNOUT3_D3D8_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Forward declarations
 * ================================================================ */

typedef struct IDirect3D8              IDirect3D8;
typedef struct IDirect3DDevice8        IDirect3DDevice8;
typedef struct IDirect3DTexture8       IDirect3DTexture8;
typedef struct IDirect3DSurface8       IDirect3DSurface8;
typedef struct IDirect3DVertexBuffer8  IDirect3DVertexBuffer8;
typedef struct IDirect3DIndexBuffer8   IDirect3DIndexBuffer8;
typedef struct IDirect3DBaseTexture8   IDirect3DBaseTexture8;
typedef struct IDirect3DCubeTexture8   IDirect3DCubeTexture8;
typedef struct IDirect3DVolumeTexture8 IDirect3DVolumeTexture8;
typedef struct IDirect3DVolume8       IDirect3DVolume8;

/* ================================================================
 * Basic D3D8 types
 * ================================================================ */

typedef DWORD D3DCOLOR;
typedef float D3DVALUE;

typedef struct D3DVECTOR {
    float x, y, z;
} D3DVECTOR;

typedef struct D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;

typedef struct D3DRECT {
    LONG x1, y1, x2, y2;
} D3DRECT;

typedef struct D3DVIEWPORT8 {
    DWORD X, Y;
    DWORD Width, Height;
    float MinZ, MaxZ;
} D3DVIEWPORT8;

typedef struct D3DLOCKED_RECT {
    INT Pitch;
    void *pBits;
} D3DLOCKED_RECT;

typedef struct D3DBOX {
    UINT Left, Top, Right, Bottom, Front, Back;
} D3DBOX;

typedef struct D3DLOCKED_BOX {
    INT RowPitch;
    INT SlicePitch;
    void *pBits;
} D3DLOCKED_BOX;

typedef struct D3DGAMMARAMP {
    WORD red[256];
    WORD green[256];
    WORD blue[256];
} D3DGAMMARAMP;

/* ================================================================
 * D3D8 enumerations
 * ================================================================ */

typedef enum D3DFORMAT {
    D3DFMT_UNKNOWN       = 0,

    /* Swizzled formats (canonical Xbox XDK values) */
    D3DFMT_L8            = 0x00,   /* 8-bit luminance */
    D3DFMT_AL8           = 0x01,   /* 8-bit alpha + luminance (packed) */
    D3DFMT_A1R5G5B5      = 0x02,
    D3DFMT_X1R5G5B5      = 0x03,
    D3DFMT_A4R4G4B4      = 0x04,
    D3DFMT_R5G6B5        = 0x05,
    D3DFMT_A8R8G8B8      = 0x06,
    D3DFMT_X8R8G8B8      = 0x07,
    D3DFMT_X8L8V8U8      = 0x08,   /* signed bump: L8 + V8U8, distinct from X8R8G8B8 */

    /* Palette formats */
    D3DFMT_P8            = 0x0B,   /* 8-bit palettized */

    /* Compressed formats */
    D3DFMT_DXT1          = 0x0C,   /* opaque / one-bit alpha */
    D3DFMT_DXT2          = 0x0E,   /* Alias for D3DFMT_DXT3 */
    D3DFMT_DXT3          = 0x0E,   /* explicit alpha */
    D3DFMT_DXT3A         = 0x59,   /* DXT3 with explicit alpha only */
    D3DFMT_DXT4          = 0x0F,   /* Alias for D3DFMT_DXT5 */
    D3DFMT_DXT5          = 0x0F,   /* interpolated alpha */
    D3DFMT_DXT5A         = 0x5A,   /* DXT5 with alpha only */
    D3DFMT_DXN           = 0x57,   /* two-channel normal-map compression (BC5-style) */
    D3DFMT_CTX1          = 0x58,   /* 2-color compressed */
    D3DFMT_Q8W8V8U8      = 0x0D,   /* signed bump: Q8W8V8U8, distinct from A8B8G8R8 */

    /* Alpha / luminance */
    D3DFMT_A8            = 0x19,
    D3DFMT_A8L8          = 0x1A,

    /* Bump map (signed) formats */
    D3DFMT_R6G5B5        = 0x27,
    D3DFMT_L6V5U5        = 0x09,   /* signed bump: L6V5U5, distinct from R6G5B5 */
    D3DFMT_G8B8          = 0x28,
    D3DFMT_V8U8          = 0x0A,   /* signed bump: V8U8, distinct from G8B8 */
    D3DFMT_R8B8          = 0x29,

    /* Depth/stencil (swizzled depth use is uncommon) */
    D3DFMT_D24S8         = 0x2A,
    D3DFMT_F24S8         = 0x2B,
    D3DFMT_D16           = 0x2C,
    D3DFMT_D16_LOCKABLE  = 0x2C,   /* Alias for D16 */
    D3DFMT_F16           = 0x2D,
    D3DFMT_D24X8         = 0x54,   /* 24-bit depth, no stencil */
    D3DFMT_D24FS8        = 0x55,   /* 24-bit float depth + 8-bit stencil */
    D3DFMT_D32           = 0x56,   /* 32-bit fixed depth */

    /* 16-bit luminance / signed */
    D3DFMT_L16           = 0x32,
    D3DFMT_V16U16        = 0x33,

    /* Channel-swapped 16-bit formats */
    D3DFMT_R5G5B5A1      = 0x38,
    D3DFMT_R4G4B4A4      = 0x39,

    /* 32-bit channel-swapped formats */
    D3DFMT_A8B8G8R8      = 0x3A,
    D3DFMT_B8G8R8A8      = 0x3B,
    D3DFMT_R8G8B8A8      = 0x3C,

    /* 10-bit formats */
    D3DFMT_A2R10G10B10   = 0x4C,
    D3DFMT_X2R10G10B10   = 0x4E,
    D3DFMT_A2B10G10R10   = 0x4F,
    D3DFMT_A2W10V10U10   = 0x50,   /* signed bump variant of A2B10G10R10 */
    D3DFMT_R10G11B11     = 0x52,
    D3DFMT_R11G11B10     = 0x53,

    /* 16/32-bit float formats */
    D3DFMT_R16F          = 0x21,
    D3DFMT_R32F          = 0x22,
    D3DFMT_G16R16F       = 0x23,
    D3DFMT_G32R32F       = 0x26,
    D3DFMT_A16B16G16R16F = 0x34,
    D3DFMT_A32B32G32R32F = 0x4B,

    /* 16/32-bit uncompressed pairs */
    D3DFMT_G16R16        = 0x42,
    D3DFMT_A16L16        = 0x43,
    D3DFMT_A16B16G16R16  = 0x44,
    D3DFMT_A32B32G32R32  = 0x45,
    D3DFMT_G32R32        = 0x46,
    D3DFMT_L32           = 0x47,
    D3DFMT_A32L32        = 0x48,

    /* Signed bump (32-bit / 64-bit) */
    D3DFMT_V32U32        = 0x49,
    D3DFMT_Q16W16V16U16  = 0x4A,
    D3DFMT_Q32W32V32U32  = 0x51,

    /* YUV formats */
    D3DFMT_YUY2          = 0x24,
    D3DFMT_UYVY          = 0x25,

    /* Vertex data (not a texture format) */
    D3DFMT_VERTEXDATA    = 0x64,

    /* ============================================================
     * Linear (unswizzled) variants of the above
     * ============================================================ */
    D3DFMT_LIN_A1R5G5B5  = 0x10,
    D3DFMT_LIN_R5G6B5    = 0x11,
    D3DFMT_LIN_A8R8G8B8  = 0x12,
    D3DFMT_LIN_L8        = 0x13,
    D3DFMT_LIN_X8L8V8U8  = 0x14,   /* signed bump: LIN_X8L8V8U8, distinct from LIN_X8R8G8B8 */
    D3DFMT_LIN_V8U8      = 0x15,   /* signed bump: LIN_V8U8, distinct from LIN_G8B8 */
    D3DFMT_LIN_R8B8      = 0x16,
    D3DFMT_LIN_G8B8      = 0x17,
    D3DFMT_LIN_L6V5U5    = 0x18,   /* signed bump: LIN_L6V5U5, distinct from LIN_R6G5B5 */
    D3DFMT_LIN_AL8       = 0x1B,
    D3DFMT_LIN_X1R5G5B5  = 0x1C,
    D3DFMT_LIN_A4R4G4B4  = 0x1D,
    D3DFMT_LIN_X8R8G8B8  = 0x1E,
    D3DFMT_LIN_A8        = 0x1F,
    D3DFMT_LIN_A8L8      = 0x20,
    D3DFMT_LIN_D24S8     = 0x2E,
    D3DFMT_LIN_F24S8     = 0x2F,
    D3DFMT_LIN_D16       = 0x30,
    D3DFMT_LIN_F16       = 0x31,
    D3DFMT_LIN_L16       = 0x35,
    D3DFMT_LIN_V16U16    = 0x36,
    D3DFMT_LIN_R6G5B5    = 0x37,
    D3DFMT_LIN_R5G5B5A1  = 0x3D,
    D3DFMT_LIN_R4G4B4A4  = 0x3E,
    D3DFMT_LIN_A8B8G8R8  = 0x3F,
    D3DFMT_LIN_B8G8R8A8  = 0x40,
    D3DFMT_LIN_R8G8B8A8  = 0x41,

    /* LIN variants of the extended (10-bit / float / 16-32-bit) formats */
    D3DFMT_LIN_R16F          = 0x5B,
    D3DFMT_LIN_R32F          = 0x5C,
    D3DFMT_LIN_G16R16F       = 0x5D,
    D3DFMT_LIN_G32R32F       = 0x5E,
    D3DFMT_LIN_A16B16G16R16F = 0x5F,
    D3DFMT_LIN_A32B32G32R32F = 0x60,
    D3DFMT_LIN_G16R16        = 0x61,
    D3DFMT_LIN_A16L16        = 0x62,
    D3DFMT_LIN_A16B16G16R16  = 0x63,
    D3DFMT_LIN_A32B32G32R32  = 0x79,
    D3DFMT_LIN_G32R32        = 0x7A,
    D3DFMT_LIN_L32           = 0x67,
    D3DFMT_LIN_A32L32        = 0x68,
    D3DFMT_LIN_V32U32        = 0x69,
    D3DFMT_LIN_Q16W16V16U16  = 0x6A,
    D3DFMT_LIN_Q32W32V32U32  = 0x6B,
    D3DFMT_LIN_A2R10G10B10   = 0x6C,
    D3DFMT_LIN_X2R10G10B10   = 0x6D,
    D3DFMT_LIN_A2B10G10R10   = 0x6E,
    D3DFMT_LIN_A2W10V10U10   = 0x6F,
    D3DFMT_LIN_R10G11B11     = 0x70,
    D3DFMT_LIN_R11G11B10     = 0x71,
    D3DFMT_LIN_D24X8         = 0x72,
    D3DFMT_LIN_D24FS8        = 0x73,
    D3DFMT_LIN_D32           = 0x74,
    D3DFMT_LIN_DXN           = 0x75,
    D3DFMT_LIN_DXT3A         = 0x76,
    D3DFMT_LIN_DXT5A         = 0x77,
    D3DFMT_LIN_CTX1          = 0x78,

    /* Index buffer formats (internal, not real Xbox formats) */
    D3DFMT_INDEX16        = 101,
    D3DFMT_INDEX32        = 102,
} D3DFORMAT;

typedef enum D3DPRIMITIVETYPE {
    D3DPT_POINTLIST     = 1,
    D3DPT_LINELIST      = 2,
    D3DPT_LINESTRIP     = 3,
    D3DPT_TRIANGLELIST  = 4,
    D3DPT_TRIANGLESTRIP = 5,
    D3DPT_TRIANGLEFAN   = 6,
    /* Xbox-specific */
    D3DPT_QUADLIST      = 8,
} D3DPRIMITIVETYPE;

typedef enum D3DTRANSFORMSTATETYPE {
    D3DTS_VIEW          = 2,
    D3DTS_PROJECTION    = 3,
    D3DTS_TEXTURE0      = 16,
    D3DTS_TEXTURE1      = 17,
    D3DTS_TEXTURE2      = 18,
    D3DTS_TEXTURE3      = 19,
    D3DTS_WORLD         = 256,
    D3DTS_WORLD1        = 257,
    D3DTS_WORLD2        = 258,
    D3DTS_WORLD3        = 259,
} D3DTRANSFORMSTATETYPE;

typedef enum D3DRENDERSTATETYPE {
    /* Standard D3D8 render states */
    D3DRS_ZENABLE                  = 7,
    D3DRS_FILLMODE                 = 8,
    D3DRS_SHADEMODE                = 9,
    D3DRS_ZWRITEENABLE             = 14,
    D3DRS_ALPHATESTENABLE          = 15,
    D3DRS_SRCBLEND                 = 19,
    D3DRS_DESTBLEND                = 20,
    D3DRS_CULLMODE                 = 22,
    D3DRS_ZFUNC                    = 23,
    D3DRS_ALPHAREF                 = 24,
    D3DRS_ALPHAFUNC                = 25,
    D3DRS_DITHERENABLE             = 26,
    D3DRS_ALPHABLENDENABLE         = 27,
    D3DRS_FOGENABLE                = 28,
    D3DRS_SPECULARENABLE           = 29,
    D3DRS_FOGCOLOR                 = 34,
    D3DRS_FOGTABLEMODE             = 35,
    D3DRS_FOGSTART                 = 36,
    D3DRS_FOGEND                   = 37,
    D3DRS_FOGDENSITY               = 38,
    D3DRS_EDGEANTIALIAS            = 40,
    D3DRS_RANGEFOGENABLE           = 48,
    D3DRS_STENCILENABLE            = 52,
    D3DRS_STENCILFAIL              = 53,
    D3DRS_STENCILZFAIL             = 54,
    D3DRS_STENCILPASS              = 55,
    D3DRS_STENCILFUNC              = 56,
    D3DRS_STENCILREF               = 57,
    D3DRS_STENCILMASK              = 58,
    D3DRS_STENCILWRITEMASK         = 59,
    D3DRS_TEXTUREFACTOR            = 60,
    D3DRS_WRAP0                    = 128,
    D3DRS_WRAP1                    = 129,
    D3DRS_WRAP2                    = 130,
    D3DRS_WRAP3                    = 131,
    D3DRS_LIGHTING                 = 137,
    D3DRS_AMBIENT                  = 139,
    D3DRS_FOGVERTEXMODE             = 140,
    D3DRS_COLORVERTEX              = 141,
    D3DRS_LOCALVIEWER              = 142,
    D3DRS_NORMALIZENORMALS         = 143,
    D3DRS_DIFFUSEMATERIALSOURCE    = 145,
    D3DRS_SPECULARMATERIALSOURCE   = 146,
    D3DRS_AMBIENTMATERIALSOURCE    = 147,
    D3DRS_EMISSIVEMATERIALSOURCE   = 148,
    D3DRS_VERTEXBLEND              = 151,
    D3DRS_POINTSIZE                = 154,
    D3DRS_POINTSIZE_MIN            = 155,
    D3DRS_POINTSPRITEENABLE        = 156,
    D3DRS_POINTSCALEENABLE         = 157,
    D3DRS_MULTISAMPLEANTIALIAS     = 161,
    D3DRS_MULTISAMPLEMASK          = 162,
    D3DRS_COLORWRITEENABLE         = 168,
    D3DRS_BLENDOP                  = 171,
    /* Xbox-specific render states (200+) */
    D3DRS_PSALPHAINPUTS0           = 200,
    D3DRS_PSALPHAINPUTS1           = 201,
    D3DRS_PSALPHAINPUTS2           = 202,
    D3DRS_PSALPHAINPUTS3           = 203,
    D3DRS_PSALPHAINPUTS4           = 204,
    D3DRS_PSALPHAINPUTS5           = 205,
    D3DRS_PSALPHAINPUTS6           = 206,
    D3DRS_PSALPHAINPUTS7           = 207,
    D3DRS_PSFINALCOMBINERINPUTSABCD = 208,
    D3DRS_PSFINALCOMBINERINPUTSEFG  = 209,
    D3DRS_PSRGBINPUTS0             = 210,
    D3DRS_PSRGBINPUTS1             = 211,
    D3DRS_PSRGBINPUTS2             = 212,
    D3DRS_PSRGBINPUTS3             = 213,
    D3DRS_PSRGBINPUTS4             = 214,
    D3DRS_PSRGBINPUTS5             = 215,
    D3DRS_PSRGBINPUTS6             = 216,
    D3DRS_PSRGBINPUTS7             = 217,
    D3DRS_PSRGBOUTPUTS0            = 218,
    D3DRS_PSRGBOUTPUTS1            = 219,
    D3DRS_PSRGBOUTPUTS2            = 220,
    D3DRS_PSRGBOUTPUTS3            = 221,
    D3DRS_PSRGBOUTPUTS4            = 222,
    D3DRS_PSRGBOUTPUTS5            = 223,
    D3DRS_PSRGBOUTPUTS6            = 224,
    D3DRS_PSRGBOUTPUTS7            = 225,
    D3DRS_PSALPHAOUTPUTS0          = 226,
    D3DRS_PSALPHAOUTPUTS1          = 227,
    D3DRS_PSALPHAOUTPUTS2          = 228,
    D3DRS_PSALPHAOUTPUTS3          = 229,
    D3DRS_PSALPHAOUTPUTS4          = 230,
    D3DRS_PSALPHAOUTPUTS5          = 231,
    D3DRS_PSALPHAOUTPUTS6          = 232,
    D3DRS_PSALPHAOUTPUTS7          = 233,
    D3DRS_PSCOMBINERCOUNT          = 234,
    D3DRS_PSCONSTANT0_0            = 235,
    D3DRS_PSCONSTANT0_1            = 236,
    D3DRS_PSCONSTANT0_2            = 237,
    D3DRS_PSCONSTANT0_3            = 238,
    D3DRS_PSCONSTANT0_4            = 239,
    D3DRS_PSCONSTANT0_5            = 240,
    D3DRS_PSCONSTANT0_6            = 241,
    D3DRS_PSCONSTANT0_7            = 242,
    D3DRS_PSCONSTANT1_0            = 243,
    D3DRS_PSCONSTANT1_1            = 244,
    D3DRS_PSCONSTANT1_2            = 245,
    D3DRS_PSCONSTANT1_3            = 246,
    D3DRS_PSCONSTANT1_4            = 247,
    D3DRS_PSCONSTANT1_5            = 248,
    D3DRS_PSCONSTANT1_6            = 249,
    D3DRS_PSCONSTANT1_7            = 250,
    D3DRS_PSTEXTUREMODES           = 251,
    D3DRS_PSDOTMAPPING             = 252,
    D3DRS_PSINPUTTEXTURE           = 253,
} D3DRENDERSTATETYPE;

typedef enum D3DTEXTURESTAGESTATETYPE {
    D3DTSS_COLOROP      = 1,
    D3DTSS_COLORARG1    = 2,
    D3DTSS_COLORARG2    = 3,
    D3DTSS_ALPHAOP      = 4,
    D3DTSS_ALPHAARG1    = 5,
    D3DTSS_ALPHAARG2    = 6,
    D3DTSS_BUMPENVMAT00 = 7,
    D3DTSS_BUMPENVMAT01 = 8,
    D3DTSS_BUMPENVMAT10 = 9,
    D3DTSS_BUMPENVMAT11 = 10,
    D3DTSS_TEXCOORDINDEX = 11,
    D3DTSS_ADDRESSU     = 13,
    D3DTSS_ADDRESSV     = 14,
    D3DTSS_BORDERCOLOR  = 15,
    D3DTSS_MAGFILTER    = 16,
    D3DTSS_MINFILTER    = 17,
    D3DTSS_MIPFILTER    = 18,
    D3DTSS_MIPMAPLODBIAS = 19,
    D3DTSS_MAXMIPLEVEL  = 20,
    D3DTSS_MAXANISOTROPY = 21,
    D3DTSS_COLORKEYOP   = 24,
    D3DTSS_COLORSIGN    = 25,
    D3DTSS_ALPHAKILL    = 26,
    D3DTSS_COLORARG0    = 26,
    D3DTSS_ALPHAARG0    = 27,
    D3DTSS_RESULTARG    = 28,
} D3DTEXTURESTAGESTATETYPE;

typedef enum D3DTEXTUREOP {
    D3DTOP_DISABLE              = 1,
    D3DTOP_SELECTARG1           = 2,
    D3DTOP_SELECTARG2           = 3,
    D3DTOP_MODULATE             = 4,
    D3DTOP_MODULATE2X           = 5,
    D3DTOP_MODULATE4X           = 6,
    D3DTOP_ADD                  = 7,
    D3DTOP_ADDSIGNED            = 8,
    D3DTOP_ADDSIGNED2X          = 9,
    D3DTOP_SUBTRACT             = 10,
    D3DTOP_ADDSMOOTH            = 11,
    D3DTOP_BLENDDIFFUSEALPHA    = 12,
    D3DTOP_BLENDTEXTUREALPHA    = 13,
    D3DTOP_BLENDFACTORALPHA     = 14,
    D3DTOP_BLENDCURRENTALPHA    = 15,
    D3DTOP_PREMODULATE          = 16,
    D3DTOP_DOTPRODUCT3          = 24,
    D3DTOP_MULTIPLYADD          = 25,
    D3DTOP_LERP                 = 26,
} D3DTEXTUREOP;

/* Texture argument flags (D3DTA_*) */
#define D3DTA_DIFFUSE           0x00
#define D3DTA_CURRENT           0x01
#define D3DTA_TEXTURE           0x02
#define D3DTA_TFACTOR           0x03
#define D3DTA_SPECULAR          0x04
#define D3DTA_COMPLEMENT        0x10
#define D3DTA_ALPHAREPLICATE    0x20

/* Texture coordinate generation (D3DTSS_TEXCOORDINDEX high bits) */
#define D3DTSS_TCI_PASSTHRU                      0x00000000
#define D3DTSS_TCI_CAMERASPACENORMAL             0x00010000
#define D3DTSS_TCI_CAMERASPACEPOSITION           0x00020000
#define D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR   0x00030000
#define D3DTSS_TCI_SPHEREMAP                     0x00040000
#define D3DTSS_TCI_MASK                          0x000F0000

/* Light types */
#define D3DLIGHT_POINT          1
#define D3DLIGHT_SPOT           2
#define D3DLIGHT_DIRECTIONAL    3

/* Fog modes */
#define D3DFOG_NONE             0
#define D3DFOG_EXP              1
#define D3DFOG_EXP2             2
#define D3DFOG_LINEAR           3

typedef enum D3DBLEND {
    D3DBLEND_ZERO            = 1,
    D3DBLEND_ONE             = 2,
    D3DBLEND_SRCCOLOR        = 3,
    D3DBLEND_INVSRCCOLOR     = 4,
    D3DBLEND_SRCALPHA        = 5,
    D3DBLEND_INVSRCALPHA     = 6,
    D3DBLEND_DESTALPHA       = 7,
    D3DBLEND_INVDESTALPHA    = 8,
    D3DBLEND_DESTCOLOR       = 9,
    D3DBLEND_INVDESTCOLOR    = 10,
    D3DBLEND_SRCALPHASAT     = 11,
} D3DBLEND;

typedef enum D3DCMPFUNC {
    D3DCMP_NEVER        = 1,
    D3DCMP_LESS         = 2,
    D3DCMP_EQUAL        = 3,
    D3DCMP_LESSEQUAL    = 4,
    D3DCMP_GREATER      = 5,
    D3DCMP_NOTEQUAL     = 6,
    D3DCMP_GREATEREQUAL = 7,
    D3DCMP_ALWAYS       = 8,
} D3DCMPFUNC;

typedef enum D3DCULL {
    D3DCULL_NONE = 1,
    D3DCULL_CW   = 2,
    D3DCULL_CCW  = 3,
} D3DCULL;

typedef enum D3DFILLMODE {
    D3DFILL_POINT     = 1,
    D3DFILL_WIREFRAME = 2,
    D3DFILL_SOLID     = 3,
} D3DFILLMODE;

typedef enum D3DPOOL {
    D3DPOOL_DEFAULT     = 0,
    D3DPOOL_MANAGED     = 1,
    D3DPOOL_SYSTEMMEM   = 2,
} D3DPOOL;

typedef enum D3DRESOURCETYPE {
    D3DRTYPE_NONE          = 0,
    D3DRTYPE_SURFACE       = 1,
    D3DRTYPE_VOLUME        = 2,
    D3DRTYPE_TEXTURE       = 3,
    D3DRTYPE_VOLUMETEXTURE = 4,
    D3DRTYPE_CUBETEXTURE   = 5,
    D3DRTYPE_VERTEXBUFFER  = 6,
    D3DRTYPE_INDEXBUFFER   = 7,
    D3DRTYPE_PUSHBUFFER    = 8,
    D3DRTYPE_PALETTE       = 9,
} D3DRESOURCETYPE;

typedef enum D3DCUBEMAP_FACES {
    D3DCUBEMAP_FACE_POSITIVE_X = 0,
    D3DCUBEMAP_FACE_NEGATIVE_X = 1,
    D3DCUBEMAP_FACE_POSITIVE_Y = 2,
    D3DCUBEMAP_FACE_NEGATIVE_Y = 3,
    D3DCUBEMAP_FACE_POSITIVE_Z = 4,
    D3DCUBEMAP_FACE_NEGATIVE_Z = 5,
    D3DCUBEMAP_FACE_FORCE_DWORD = 0x7FFFFFFF,
} D3DCUBEMAP_FACES;

typedef enum D3DMULTISAMPLE_TYPE {
    /* Canonical Xbox XDK values. The low two nibbles are the X/Y
     * sampling grid (sample count = X*Y); bits 12-13 select the
     * algorithm (0=none, 1=multisample, 2=supersample). */
    D3DMULTISAMPLE_NONE                                    = 0x0011,
    D3DMULTISAMPLE_2_SAMPLES_MULTISAMPLE_LINEAR            = 0x1021,
    D3DMULTISAMPLE_2_SAMPLES_MULTISAMPLE_QUINCUNX          = 0x1121,
    D3DMULTISAMPLE_2_SAMPLES_SUPERSAMPLE_HORIZONTAL_LINEAR = 0x2021,
    D3DMULTISAMPLE_2_SAMPLES_SUPERSAMPLE_VERTICAL_LINEAR   = 0x2012,
    D3DMULTISAMPLE_4_SAMPLES_MULTISAMPLE_LINEAR            = 0x1022,
    D3DMULTISAMPLE_4_SAMPLES_MULTISAMPLE_GAUSSIAN          = 0x1222,
    D3DMULTISAMPLE_4_SAMPLES_SUPERSAMPLE_LINEAR            = 0x2022,
    D3DMULTISAMPLE_4_SAMPLES_SUPERSAMPLE_GAUSSIAN          = 0x2222,
    D3DMULTISAMPLE_9_SAMPLES_MULTISAMPLE_GAUSSIAN          = 0x1233,
    D3DMULTISAMPLE_9_SAMPLES_SUPERSAMPLE_GAUSSIAN          = 0x2233,

    /* Legacy shorthand aliases used by older toolkit code. */
    D3DMULTISAMPLE_2_SAMPLES = D3DMULTISAMPLE_2_SAMPLES_MULTISAMPLE_LINEAR,
    D3DMULTISAMPLE_4_SAMPLES = D3DMULTISAMPLE_4_SAMPLES_MULTISAMPLE_LINEAR,
} D3DMULTISAMPLE_TYPE;

/* Resolve an Xbox multisample type to a D3D11 sample count.
 * NONE (0x0011) yields 1; 2/4/9-sample grids map to their X*Y counts. */
static inline UINT d3d8_msaa_sample_count(D3DMULTISAMPLE_TYPE ms)
{
    UINT xs = (UINT)(((unsigned)ms >> 4) & 0xF);
    UINT ys = (UINT)ms & 0xF;
    UINT count = xs ? xs : 1;
    count *= ys ? ys : 1;
    return count < 1 ? 1 : count;
}

typedef enum D3DTEXTUREFILTERTYPE {
    D3DTEXF_NONE            = 0,
    D3DTEXF_POINT           = 1,
    D3DTEXF_LINEAR          = 2,
    D3DTEXF_ANISOTROPIC     = 3,
    D3DTEXF_QUINCUNX        = 4,  /* Xbox-specific */
    D3DTEXF_GAUSSIANCUBIC   = 5,  /* Xbox-specific */
} D3DTEXTUREFILTERTYPE;

typedef enum D3DTEXTUREADDRESS {
    D3DTADDRESS_WRAP        = 1,
    D3DTADDRESS_MIRROR      = 2,
    D3DTADDRESS_CLAMP       = 3,
    D3DTADDRESS_BORDER      = 4,
    D3DTADDRESS_MIRRORONCE  = 5,
} D3DTEXTUREADDRESS;

typedef enum D3DSWAPEFFECT {
    D3DSWAPEFFECT_DISCARD = 1,
    D3DSWAPEFFECT_FLIP    = 2,
    D3DSWAPEFFECT_COPY    = 3,
} D3DSWAPEFFECT;

typedef enum D3DCLEAR_FLAGS {
    D3DCLEAR_TARGET  = 0x01,
    D3DCLEAR_ZBUFFER = 0x02,
    D3DCLEAR_STENCIL = 0x04,
} D3DCLEAR_FLAGS;

/* ================================================================
 * Vertex declaration / FVF
 * ================================================================ */

#define D3DFVF_POSITION_MASK    0x00E
#define D3DFVF_XYZ              0x002
#define D3DFVF_XYZRHW           0x004
#define D3DFVF_XYZB1            0x006
#define D3DFVF_XYZB2            0x008
#define D3DFVF_XYZB3            0x00A
#define D3DFVF_XYZB4            0x00C
#define D3DFVF_XYZB5            0x00E
#define D3DFVF_NORMAL           0x010
#define D3DFVF_DIFFUSE          0x040
#define D3DFVF_SPECULAR         0x080
#define D3DFVF_TEX0             0x000
#define D3DFVF_TEX1             0x100
#define D3DFVF_TEX2             0x200
#define D3DFVF_TEX3             0x300
#define D3DFVF_TEX4             0x400
#define D3DFVF_TEXCOUNT_MASK    0xF00
#define D3DFVF_TEXCOUNT_SHIFT   8

/* Per-texcoord-set component count (number of floats). A 2-bit field per
 * set, starting at bit 16. 0 means the default of 2. Encoded as:
 *   field = (fvf >> (tcoordsize_field_shift + t*2)) & 0x3
 * On Xbox the convention is D3DFVF_TEXCOORDSIZE1..4 shifting each set by 2.
 */
#define D3DFVF_TEXCOORDSIZE1     0x10000   /* 1 float  (set 0) */
#define D3DFVF_TEXCOORDSIZE2     0x00000   /* 2 floats (set 0, default) */
#define D3DFVF_TEXCOORDSIZE3     0x20000   /* 3 floats (set 0) */
#define D3DFVF_TEXCOORDSIZE4     0x30000   /* 4 floats (set 0) */
#define D3DFVF_TEXCOORDSIZE_MASK 0xFFFF0000

/* ================================================================
 * Structures
 * ================================================================ */

typedef struct D3DPRESENT_PARAMETERS {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    D3DFORMAT BackBufferFormat;
    UINT BackBufferCount;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    D3DSWAPEFFECT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    D3DFORMAT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
} D3DPRESENT_PARAMETERS;

typedef struct D3DMATERIAL8 {
    struct { float r, g, b, a; } Diffuse;
    struct { float r, g, b, a; } Ambient;
    struct { float r, g, b, a; } Specular;
    struct { float r, g, b, a; } Emissive;
    float Power;
} D3DMATERIAL8;

typedef struct D3DLIGHT8 {
    DWORD Type;
    struct { float r, g, b, a; } Diffuse;
    struct { float r, g, b, a; } Specular;
    struct { float r, g, b, a; } Ambient;
    D3DVECTOR Position;
    D3DVECTOR Direction;
    float Range;
    float Falloff;
    float Attenuation0;
    float Attenuation1;
    float Attenuation2;
    float Theta;
    float Phi;
} D3DLIGHT8;

typedef struct D3DSURFACE_DESC {
    D3DFORMAT Format;
    DWORD Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width;
    UINT Height;
} D3DSURFACE_DESC;

typedef struct D3DVOLUME_DESC {
    D3DFORMAT Format;
    DWORD Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    UINT Width;
    UINT Height;
    UINT Depth;
} D3DVOLUME_DESC;

/* ================================================================
 * Lock flags
 * ================================================================ */

#define D3DLOCK_READONLY    0x00000010
#define D3DLOCK_DISCARD     0x00002000
#define D3DLOCK_NOOVERWRITE 0x00001000
#define D3DLOCK_NOSYSLOCK   0x00000800

/* ================================================================
 * Usage flags
 * ================================================================ */

#define D3DUSAGE_RENDERTARGET       0x00000001
#define D3DUSAGE_DEPTHSTENCIL       0x00000002
#define D3DUSAGE_WRITEONLY          0x00000008
#define D3DUSAGE_DYNAMIC            0x00000200

/* ================================================================
 * Xbox-specific: Push buffer types
 * ================================================================ */

typedef struct D3DPushBuffer {
    DWORD Common;
    DWORD Data;
    DWORD Size;
    DWORD AllocationSize;
} D3DPushBuffer;

/* ================================================================
 * IDirect3DVertexBuffer8 interface
 * ================================================================ */

typedef struct IDirect3DVertexBuffer8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DVertexBuffer8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DVertexBuffer8 *self);
    ULONG   (__stdcall *Release)(IDirect3DVertexBuffer8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DVertexBuffer8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DVertexBuffer8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DVertexBuffer8 *self);
    void    (__stdcall *PreLoad)(IDirect3DVertexBuffer8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DVertexBuffer8 *self);
    /* IDirect3DVertexBuffer8 */
    HRESULT (__stdcall *Lock)(IDirect3DVertexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags);
    HRESULT (__stdcall *Unlock)(IDirect3DVertexBuffer8 *self);
    HRESULT (__stdcall *GetDesc)(IDirect3DVertexBuffer8 *self, void *pDesc);
} IDirect3DVertexBuffer8Vtbl;

struct IDirect3DVertexBuffer8 {
    const IDirect3DVertexBuffer8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DIndexBuffer8 interface
 * ================================================================ */

typedef struct IDirect3DIndexBuffer8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DIndexBuffer8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DIndexBuffer8 *self);
    ULONG   (__stdcall *Release)(IDirect3DIndexBuffer8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DIndexBuffer8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DIndexBuffer8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DIndexBuffer8 *self);
    void    (__stdcall *PreLoad)(IDirect3DIndexBuffer8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DIndexBuffer8 *self);
    /* IDirect3DIndexBuffer8 */
    HRESULT (__stdcall *Lock)(IDirect3DIndexBuffer8 *self, UINT OffsetToLock, UINT SizeToLock, BYTE **ppbData, DWORD Flags);
    HRESULT (__stdcall *Unlock)(IDirect3DIndexBuffer8 *self);
    HRESULT (__stdcall *GetDesc)(IDirect3DIndexBuffer8 *self, void *pDesc);
} IDirect3DIndexBuffer8Vtbl;

struct IDirect3DIndexBuffer8 {
    const IDirect3DIndexBuffer8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DTexture8 interface
 * ================================================================ */

typedef struct IDirect3DTexture8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DTexture8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DTexture8 *self);
    ULONG   (__stdcall *Release)(IDirect3DTexture8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DTexture8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DTexture8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DTexture8 *self);
    void    (__stdcall *PreLoad)(IDirect3DTexture8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DTexture8 *self);
    /* IDirect3DBaseTexture8 */
    DWORD   (__stdcall *GetLevelCount)(IDirect3DTexture8 *self);
    /* IDirect3DTexture8 */
    HRESULT (__stdcall *GetLevelDesc)(IDirect3DTexture8 *self, UINT Level, D3DSURFACE_DESC *pDesc);
    HRESULT (__stdcall *GetSurfaceLevel)(IDirect3DTexture8 *self, UINT Level, IDirect3DSurface8 **ppSurface);
    HRESULT (__stdcall *LockRect)(IDirect3DTexture8 *self, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
    HRESULT (__stdcall *UnlockRect)(IDirect3DTexture8 *self, UINT Level);
} IDirect3DTexture8Vtbl;

struct IDirect3DTexture8 {
    const IDirect3DTexture8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DSurface8 interface
 * ================================================================ */

typedef struct IDirect3DSurface8Vtbl {
    HRESULT (__stdcall *QueryInterface)(IDirect3DSurface8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DSurface8 *self);
    ULONG   (__stdcall *Release)(IDirect3DSurface8 *self);
    HRESULT (__stdcall *GetDevice)(IDirect3DSurface8 *self, IDirect3DDevice8 **ppDevice);
    HRESULT (__stdcall *GetDesc)(IDirect3DSurface8 *self, D3DSURFACE_DESC *pDesc);
    HRESULT (__stdcall *LockRect)(IDirect3DSurface8 *self, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
    HRESULT (__stdcall *UnlockRect)(IDirect3DSurface8 *self);
} IDirect3DSurface8Vtbl;

struct IDirect3DSurface8 {
    const IDirect3DSurface8Vtbl *lpVtbl;
};

#ifdef COBJMACROS
#define IDirect3DSurface8_QueryInterface(This,riid,ppv) \
    (This)->lpVtbl->QueryInterface((This),(riid),(ppv))
#define IDirect3DSurface8_AddRef(This) \
    (This)->lpVtbl->AddRef((This))
#define IDirect3DSurface8_Release(This) \
    (This)->lpVtbl->Release((This))
#define IDirect3DSurface8_GetDevice(This,ppDevice) \
    (This)->lpVtbl->GetDevice((This),(ppDevice))
#define IDirect3DSurface8_GetDesc(This,pDesc) \
    (This)->lpVtbl->GetDesc((This),(pDesc))
#define IDirect3DSurface8_LockRect(This,pLockedRect,pRect,Flags) \
    (This)->lpVtbl->LockRect((This),(pLockedRect),(pRect),(Flags))
#define IDirect3DSurface8_UnlockRect(This) \
    (This)->lpVtbl->UnlockRect((This))
#endif

/* ================================================================
 * IDirect3DCubeTexture8 interface
 * ================================================================ */

typedef struct IDirect3DCubeTexture8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DCubeTexture8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DCubeTexture8 *self);
    ULONG   (__stdcall *Release)(IDirect3DCubeTexture8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DCubeTexture8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DCubeTexture8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DCubeTexture8 *self);
    void    (__stdcall *PreLoad)(IDirect3DCubeTexture8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DCubeTexture8 *self);
    /* IDirect3DBaseTexture8 */
    DWORD   (__stdcall *GetLevelCount)(IDirect3DCubeTexture8 *self);
    /* IDirect3DCubeTexture8 */
    HRESULT (__stdcall *GetLevelDesc)(IDirect3DCubeTexture8 *self, UINT Level, D3DSURFACE_DESC *pDesc);
    HRESULT (__stdcall *GetCubeMapSurface)(IDirect3DCubeTexture8 *self, D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface8 **ppCubeMapSurface);
    HRESULT (__stdcall *LockRect)(IDirect3DCubeTexture8 *self, D3DCUBEMAP_FACES FaceType, UINT Level, D3DLOCKED_RECT *pLockedRect, const RECT *pRect, DWORD Flags);
    HRESULT (__stdcall *UnlockRect)(IDirect3DCubeTexture8 *self, D3DCUBEMAP_FACES FaceType, UINT Level);
} IDirect3DCubeTexture8Vtbl;

struct IDirect3DCubeTexture8 {
    const IDirect3DCubeTexture8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DVolume interface
 * ================================================================ */

typedef struct IDirect3DVolume8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DVolume8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DVolume8 *self);
    ULONG   (__stdcall *Release)(IDirect3DVolume8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DVolume8 *self, IDirect3DDevice8 **ppDevice);
    HRESULT (__stdcall *GetContainer)(IDirect3DVolume8 *self, const IID *riid, void **ppContainer);
    /* IDirect3DVolume8 */
    HRESULT (__stdcall *GetDesc)(IDirect3DVolume8 *self, D3DVOLUME_DESC *pDesc);
    HRESULT (__stdcall *LockBox)(IDirect3DVolume8 *self, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags);
    HRESULT (__stdcall *UnlockBox)(IDirect3DVolume8 *self);
} IDirect3DVolume8Vtbl;

struct IDirect3DVolume8 {
    const IDirect3DVolume8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DVolumeTexture8 interface
 * ================================================================ */

typedef struct IDirect3DVolumeTexture8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DVolumeTexture8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DVolumeTexture8 *self);
    ULONG   (__stdcall *Release)(IDirect3DVolumeTexture8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DVolumeTexture8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DVolumeTexture8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DVolumeTexture8 *self);
    void    (__stdcall *PreLoad)(IDirect3DVolumeTexture8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DVolumeTexture8 *self);
    /* IDirect3DBaseTexture8 */
    DWORD   (__stdcall *GetLevelCount)(IDirect3DVolumeTexture8 *self);
    /* IDirect3DVolumeTexture8 */
    HRESULT (__stdcall *GetLevelDesc)(IDirect3DVolumeTexture8 *self, UINT Level, D3DVOLUME_DESC *pDesc);
    HRESULT (__stdcall *GetVolumeLevel)(IDirect3DVolumeTexture8 *self, UINT Level, IDirect3DVolume8 **ppVolume);
    HRESULT (__stdcall *LockBox)(IDirect3DVolumeTexture8 *self, UINT Level, D3DLOCKED_BOX *pLockedVolume, const D3DBOX *pBox, DWORD Flags);
    HRESULT (__stdcall *UnlockBox)(IDirect3DVolumeTexture8 *self, UINT Level);
} IDirect3DVolumeTexture8Vtbl;

struct IDirect3DVolumeTexture8 {
    const IDirect3DVolumeTexture8Vtbl *lpVtbl;
};

/* ================================================================
 * IDirect3DBaseTexture8 interface (common subset used by SetTexture)
 * ================================================================ */

typedef struct IDirect3DBaseTexture8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DBaseTexture8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DBaseTexture8 *self);
    ULONG   (__stdcall *Release)(IDirect3DBaseTexture8 *self);
    /* IDirect3DResource8 */
    HRESULT (__stdcall *GetDevice)(IDirect3DBaseTexture8 *self, IDirect3DDevice8 **ppDevice);
    DWORD   (__stdcall *SetPriority)(IDirect3DBaseTexture8 *self, DWORD Priority);
    DWORD   (__stdcall *GetPriority)(IDirect3DBaseTexture8 *self);
    void    (__stdcall *PreLoad)(IDirect3DBaseTexture8 *self);
    DWORD   (__stdcall *GetType)(IDirect3DBaseTexture8 *self);
    /* IDirect3DBaseTexture8 */
    DWORD   (__stdcall *GetLevelCount)(IDirect3DBaseTexture8 *self);
} IDirect3DBaseTexture8Vtbl;

struct IDirect3DBaseTexture8 {
    const IDirect3DBaseTexture8Vtbl *lpVtbl;
};

#ifdef COBJMACROS
#define IDirect3DBaseTexture8_AddRef(This) \
    (This)->lpVtbl->AddRef((This))
#define IDirect3DBaseTexture8_Release(This) \
    (This)->lpVtbl->Release((This))
#define IDirect3DBaseTexture8_GetType(This) \
    (This)->lpVtbl->GetType((This))
#define IDirect3DBaseTexture8_GetLevelCount(This) \
    (This)->lpVtbl->GetLevelCount((This))
#define IDirect3DCubeTexture8_AddRef(This) \
    (This)->lpVtbl->AddRef((This))
#define IDirect3DCubeTexture8_Release(This) \
    (This)->lpVtbl->Release((This))
#define IDirect3DCubeTexture8_GetLevelCount(This) \
    (This)->lpVtbl->GetLevelCount((This))
#define IDirect3DCubeTexture8_GetCubeMapSurface(This,Face,Level,pp) \
    (This)->lpVtbl->GetCubeMapSurface((This),(Face),(Level),(pp))
#define IDirect3DCubeTexture8_LockRect(This,Face,Level,pRect2,pRect,Flags) \
    (This)->lpVtbl->LockRect((This),(Face),(Level),(pRect2),(pRect),(Flags))
#define IDirect3DCubeTexture8_UnlockRect(This,Face,Level) \
    (This)->lpVtbl->UnlockRect((This),(Face),(Level))
#define IDirect3DVolume8_AddRef(This) \
    (This)->lpVtbl->AddRef((This))
#define IDirect3DVolume8_Release(This) \
    (This)->lpVtbl->Release((This))
#define IDirect3DVolume8_GetDesc(This,pDesc) \
    (This)->lpVtbl->GetDesc((This),(pDesc))
#define IDirect3DVolume8_LockBox(This,pLocked,pBox,Flags) \
    (This)->lpVtbl->LockBox((This),(pLocked),(pBox),(Flags))
#define IDirect3DVolume8_UnlockBox(This) \
    (This)->lpVtbl->UnlockBox((This))
#define IDirect3DVolumeTexture8_AddRef(This) \
    (This)->lpVtbl->AddRef((This))
#define IDirect3DVolumeTexture8_Release(This) \
    (This)->lpVtbl->Release((This))
#define IDirect3DVolumeTexture8_LockBox(This,Level,pLocked,pBox,Flags) \
    (This)->lpVtbl->LockBox((This),(Level),(pLocked),(pBox),(Flags))
#define IDirect3DVolumeTexture8_UnlockBox(This,Level) \
    (This)->lpVtbl->UnlockBox((This),(Level))
#endif

/* ================================================================
 * IDirect3DDevice8 interface (COM vtable)
 *
 * This is the Xbox variant. The vtable layout matches the Xbox
 * D3D8 binary so that translated code can call through it.
 * ================================================================ */

typedef struct IDirect3DDevice8Vtbl {
    /* IUnknown */
    HRESULT (__stdcall *QueryInterface)(IDirect3DDevice8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3DDevice8 *self);
    ULONG   (__stdcall *Release)(IDirect3DDevice8 *self);

    /* IDirect3DDevice8 core */
    HRESULT (__stdcall *GetDirect3D)(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8);
    HRESULT (__stdcall *GetDeviceCaps)(IDirect3DDevice8 *self, void *pCaps);
    HRESULT (__stdcall *GetDisplayMode)(IDirect3DDevice8 *self, void *pMode);
    HRESULT (__stdcall *GetCreationParameters)(IDirect3DDevice8 *self, void *pParams);

    /* Rendering */
    HRESULT (__stdcall *Reset)(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP);
    HRESULT (__stdcall *Present)(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty);
    HRESULT (__stdcall *GetBackBuffer)(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface);

    /* Scene management */
    HRESULT (__stdcall *BeginScene)(IDirect3DDevice8 *self);
    HRESULT (__stdcall *EndScene)(IDirect3DDevice8 *self);
    HRESULT (__stdcall *Clear)(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil);

    /* Transforms */
    HRESULT (__stdcall *SetTransform)(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix);
    HRESULT (__stdcall *GetTransform)(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix);

    /* Render state */
    HRESULT (__stdcall *SetRenderState)(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value);
    HRESULT (__stdcall *GetRenderState)(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue);

    /* Texture state */
    HRESULT (__stdcall *SetTextureStageState)(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
    HRESULT (__stdcall *GetTextureStageState)(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue);

    /* Textures */
    HRESULT (__stdcall *SetTexture)(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture);
    HRESULT (__stdcall *GetTexture)(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture);

    /* Vertex/index buffers */
    HRESULT (__stdcall *SetStreamSource)(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride);
    HRESULT (__stdcall *GetStreamSource)(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride);
    HRESULT (__stdcall *SetIndices)(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex);
    HRESULT (__stdcall *GetIndices)(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex);

    /* Drawing */
    HRESULT (__stdcall *DrawPrimitive)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount);
    HRESULT (__stdcall *DrawIndexedPrimitive)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount);
    HRESULT (__stdcall *DrawPrimitiveUP)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride);
    HRESULT (__stdcall *DrawIndexedPrimitiveUP)(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride);

    /* Resource creation */
    HRESULT (__stdcall *CreateTexture)(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture);
    HRESULT (__stdcall *CreateVertexBuffer)(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer);
    HRESULT (__stdcall *CreateIndexBuffer)(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer);
    HRESULT (__stdcall *CreateRenderTarget)(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface);
    HRESULT (__stdcall *CreateDepthStencilSurface)(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface);

    /* Render targets */
    HRESULT (__stdcall *SetRenderTarget)(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface);
    HRESULT (__stdcall *GetRenderTarget)(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget);
    HRESULT (__stdcall *GetDepthStencilSurface)(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface);

    /* Viewport */
    HRESULT (__stdcall *SetViewport)(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport);
    HRESULT (__stdcall *GetViewport)(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport);

    /* Material / Lighting */
    HRESULT (__stdcall *SetMaterial)(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial);
    HRESULT (__stdcall *GetMaterial)(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial);
    HRESULT (__stdcall *SetLight)(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight);
    HRESULT (__stdcall *GetLight)(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight);
    HRESULT (__stdcall *LightEnable)(IDirect3DDevice8 *self, DWORD Index, BOOL Enable);

    /* Shaders */
    HRESULT (__stdcall *SetVertexShader)(IDirect3DDevice8 *self, DWORD Handle);
    HRESULT (__stdcall *GetVertexShader)(IDirect3DDevice8 *self, DWORD *pHandle);
    HRESULT (__stdcall *SetVertexShaderConstant)(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount);
    HRESULT (__stdcall *SetPixelShader)(IDirect3DDevice8 *self, DWORD Handle);
    HRESULT (__stdcall *GetPixelShader)(IDirect3DDevice8 *self, DWORD *pHandle);
    HRESULT (__stdcall *SetPixelShaderConstant)(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount);

    /* Gamma */
    void    (__stdcall *SetGammaRamp)(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp);
    void    (__stdcall *GetGammaRamp)(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp);

    /* Palette (Xbox-specific) */
    HRESULT (__stdcall *SetPalette)(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries);

    /* Xbox-specific: push buffer */
    HRESULT (__stdcall *BeginPush)(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush);
    HRESULT (__stdcall *EndPush)(IDirect3DDevice8 *self, DWORD *pPush);

    /* Swap / display */
    HRESULT (__stdcall *Swap)(IDirect3DDevice8 *self, DWORD Flags);

    /* ============================================================
     * Extensions appended by xboxrecomp (kept after the historical
     * slots so existing slot indices are unchanged).
     * ============================================================ */
    HRESULT (__stdcall *CreateImageSurface)(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface);
    HRESULT (__stdcall *CreateCubeTexture)(IDirect3DDevice8 *self, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture);
    HRESULT (__stdcall *CreateVolumeTexture)(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture);
} IDirect3DDevice8Vtbl;

struct IDirect3DDevice8 {
    const IDirect3DDevice8Vtbl *lpVtbl;
};

/* Convenience macros for COM-style method calls */
#define IDirect3DDevice8_SetRenderState(p,s,v)       (p)->lpVtbl->SetRenderState(p,s,v)
#define IDirect3DDevice8_SetTransform(p,s,m)          (p)->lpVtbl->SetTransform(p,s,m)
#define IDirect3DDevice8_SetTexture(p,s,t)            (p)->lpVtbl->SetTexture(p,s,t)
#define IDirect3DDevice8_DrawPrimitive(p,t,sv,pc)     (p)->lpVtbl->DrawPrimitive(p,t,sv,pc)
#define IDirect3DDevice8_DrawIndexedPrimitive(p,t,mi,nv,si,pc) (p)->lpVtbl->DrawIndexedPrimitive(p,t,mi,nv,si,pc)
#define IDirect3DDevice8_Clear(p,n,r,f,c,z,s)         (p)->lpVtbl->Clear(p,n,r,f,c,z,s)
#define IDirect3DDevice8_Present(p,s,d,w,pd)          (p)->lpVtbl->Present(p,s,d,w,pd)
#define IDirect3DDevice8_BeginScene(p)                (p)->lpVtbl->BeginScene(p)
#define IDirect3DDevice8_EndScene(p)                  (p)->lpVtbl->EndScene(p)

/* ================================================================
 * IDirect3D8 interface (factory)
 * ================================================================ */

typedef struct IDirect3D8Vtbl {
    HRESULT (__stdcall *QueryInterface)(IDirect3D8 *self, const IID *riid, void **ppv);
    ULONG   (__stdcall *AddRef)(IDirect3D8 *self);
    ULONG   (__stdcall *Release)(IDirect3D8 *self);
    HRESULT (__stdcall *CreateDevice)(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice);
} IDirect3D8Vtbl;

struct IDirect3D8 {
    const IDirect3D8Vtbl *lpVtbl;
};

/* ================================================================
 * Initialization
 * ================================================================ */

/**
 * Create the D3D8-compatible interface backed by D3D11.
 * This replaces the Xbox Direct3DCreate8() call.
 */
IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion);

/**
 * Set the window title used by the D3D8 GL backend (POSIX builds) when it
 * creates its own SDL window. The Win32 backend renders into a host-provided
 * HWND and does not use this. Passing NULL restores the generic default.
 */
void xbox_D3D8SetWindowTitle(const char *title);

/**
 * Get the current D3D device (Xbox uses a global device pointer).
 */
IDirect3DDevice8 *xbox_GetD3DDevice(void);

/**
 * Present frame and pump window messages.
 * Called from recompiled game code (replaces RW driver Present path).
 */
void d3d8_PresentFrame(void);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_D3D8_XBOX_H */
