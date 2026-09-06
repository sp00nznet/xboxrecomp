/**
 * D3D8 Fixed-Function Pipeline Emulation via D3D11 Shaders
 *
 * Emulates the Xbox D3D8 fixed-function pipeline using D3D11
 * programmable shaders. Handles:
 *   - FVF-based vertex formats (XYZ, XYZRHW, Normal, Diffuse, Specular, TexCoord×4)
 *   - World/View/Projection transform
 *   - Pre-transformed vertex passthrough (XYZRHW)
 *   - Up to 4 texture stages with full D3D8 texture operations
 *   - Hardware T&L lighting (up to 8 directional/point/spot lights)
 *   - Vertex fog (linear/exp/exp2)
 *   - Alpha test
 *
 * Shader source is compiled at init time using D3DCompile.
 * When NV2A register combiners are active, the combiner module
 * overrides the pixel shader (d3d8_combiners.c).
 */

#include "d3d8_internal.h"
#include "d3d8_fvf.h"
#include <d3dcompiler.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#pragma comment(lib, "d3dcompiler.lib")

/* ================================================================
 * HLSL vertex shader (embedded)
 *
 * Supports:
 * - Standard transform (WVP) or pre-transformed (XYZRHW) passthrough
 * - Up to 4 texture coordinates
 * - Diffuse + specular vertex colors
 * - D3D8 hardware lighting (up to 8 lights)
 * - Vertex fog (linear/exp/exp2)
 * ================================================================ */

static const char g_vs_source[] =
    "cbuffer TransformCB : register(b0) {\n"
    "    float4x4 WorldViewProj;\n"
    "    float4x4 World;\n"
    "    float4x4 WorldInvTranspose;\n"
    "    float2   ScreenSize;\n"
    "    uint     Flags;\n"
    "    float    _pad0;\n"
    "    float4   EyePos;\n"
    "    float4   FogParams;\n"  /* x=start, y=end, z=density, w=vertex fog mode (0=none,1=exp,2=exp2,3=linear) */
    "    float4x4 WorldView;\n"             /* world*view (camera space) */
    "    float4x4 WorldViewInvTranspose;\n" /* inverse-transpose(world*view), for camera-space normals */
    "    float4x4 TexMat[4];\n"             /* per-stage texture matrices */
    "    uint4    TexCoordIndex;\n"         /* per-stage D3DTSS_TEXCOORDINDEX (TCI + set select) */
    "};\n"
    "\n"
    "cbuffer LightingCB : register(b1) {\n"
    "    float4 GlobalAmbient;\n"
    "    float4 MatDiffuse;\n"
    "    float4 MatAmbient;\n"
    "    float4 MatSpecular;\n"
    "    float4 MatEmissive;\n"
    "    float  MatPower;\n"
    "    uint   NumLights;\n"
    "    float2 _lpad0;\n"
    "    struct {\n"
    "        float4 PosType;\n"     /* xyz=position, w=type (1=point,2=spot,3=dir) */
    "        float4 DirRange;\n"    /* xyz=direction, w=range */
    "        float4 Diffuse;\n"
    "        float4 Ambient;\n"
    "        float4 Specular;\n"
    "        float4 Atten;\n"       /* x=const, y=linear, z=quadratic, w=falloff */
    "        float4 Spot;\n"        /* x=cos(theta/2), y=cos(phi/2), z=0, w=0 */
    "    } Lights[8];\n"
    "};\n"
    "\n"
    "struct VS_IN {\n"
    "    float4 pos      : POSITION;\n"
    "    float3 normal   : NORMAL;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float4 tex0     : TEXCOORD0;\n"
    "    float4 tex1     : TEXCOORD1;\n"
    "    float4 tex2     : TEXCOORD2;\n"
    "    float4 tex3     : TEXCOORD3;\n"
    "};\n"
    "\n"
    "struct VS_OUT {\n"
    "    float4 pos      : SV_POSITION;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float3 tex0     : TEXCOORD0;\n"
    "    float3 tex1     : TEXCOORD1;\n"
    "    float3 tex2     : TEXCOORD2;\n"
    "    float3 tex3     : TEXCOORD3;\n"
    "    float  fog      : TEXCOORD4;\n"
    "    float4 viewpos  : TEXCOORD5;\n"   /* view-space position (per-pixel fog) */
    "};\n"
    "\n"
    "// Flags: bit0=pretransformed, bit1=hasDiffuse, bit2=hasSpecular, bit3=hasNormal\n"
    "//        bit4=lighting, bit8-11=texCount\n"
    "#define FLAG_PRETRANSFORMED 0x01u\n"
    "#define FLAG_HAS_DIFFUSE    0x02u\n"
    "#define FLAG_HAS_SPECULAR   0x04u\n"
    "#define FLAG_HAS_NORMAL     0x08u\n"
    "#define FLAG_LIGHTING       0x10u\n"
    "\n"
    "// D3DFOGMODE: 0=NONE, 1=EXP, 2=EXP2, 3=LINEAR\n"
    "float compute_fog(float dist) {\n"
    "    uint mode = (uint)FogParams.w;\n"
    "    if (mode == 1u) {\n"  /* EXP */
    "        return saturate(exp(-FogParams.z * dist));\n"
    "    } else if (mode == 2u) {\n"  /* EXP2 */
    "        float e = FogParams.z * dist;\n"
    "        return saturate(exp(-e * e));\n"
    "    } else if (mode == 3u) {\n"  /* LINEAR */
    "        return saturate((FogParams.y - dist) / max(FogParams.y - FogParams.x, 1e-6));\n"
    "    }\n"
    "    return 1.0;\n"  /* no fog */
    "}\n"
    "\n"
    // Generate the texture coordinate for a stage per D3DTSS_TEXCOORDINDEX.
    // Low 16 bits = source texcoord set (PASSTHRU), high bits = TCI mode.
    "float4 gen_tex(uint stage, VS_IN input, float3 posCam, float3 nrmCam) {\n"
    "    uint tci = TexCoordIndex[stage];\n"
    "    uint mode = tci & 0x000F0000u;\n"
    "    uint src  = tci & 0x0000FFFFu;\n"
    "    float4 uv;\n"
    "    if (mode == 0x00010000u) {\n"       /* TCI_CAMERASPACENORMAL */
    "        uv = float4(nrmCam, 1.0);\n"
    "    } else if (mode == 0x00020000u) {\n"   /* TCI_CAMERASPACEPOSITION */
    "        uv = float4(posCam, 1.0);\n"
    "    } else if (mode == 0x00030000u) {\n"   /* TCI_CAMERASPACEREFLECTIONVECTOR */
    "        float3 V = normalize(-posCam);\n"
    "        float3 N = normalize(nrmCam);\n"
    "        uv = float4(reflect(V, N), 1.0);\n"
    "    } else if (mode == 0x00040000u) {\n"   /* TCI_SPHEREMAP */
    "        float3 n = normalize(nrmCam);\n"
    "        float m = 2.0 * sqrt(n.x*n.x + n.y*n.y + (n.z + 1.0)*(n.z + 1.0));\n"
    "        uv = float4(n.x / m + 0.5, n.y / m + 0.5, 0.0, 1.0);\n"
    "    } else {\n"                             /* TCI_PASSTHRU: use texcoord set [src] */
    "        if (src == 1u) uv = input.tex1;\n"
    "        else if (src == 2u) uv = input.tex2;\n"
    "        else if (src == 3u) uv = input.tex3;\n"
    "        else               uv = input.tex0;\n"
    "    }\n"
    "    return mul(uv, TexMat[stage]);\n"
    "}\n"
    "\n"
    "VS_OUT main(VS_IN input) {\n"
    "    VS_OUT o;\n"
    "    o.fog = 1.0;\n"
    "    o.specular = float4(0, 0, 0, 0);\n"
    "    o.viewpos = float4(0, 0, 0, 1);\n"
    "\n"
    "    if (Flags & FLAG_PRETRANSFORMED) {\n"
    "        o.pos.x = (input.pos.x / ScreenSize.x) * 2.0 - 1.0;\n"
    "        o.pos.y = 1.0 - (input.pos.y / ScreenSize.y) * 2.0;\n"
    "        o.pos.z = input.pos.z;\n"
    "        o.pos.w = 1.0;\n"
    "        o.tex0 = input.tex0.xyz;\n"
    "        o.tex1 = input.tex1.xyz;\n"
    "        o.tex2 = input.tex2.xyz;\n"
    "        o.tex3 = input.tex3.xyz;\n"
    "        o.diffuse = (Flags & FLAG_HAS_DIFFUSE) ? input.diffuse.bgra : float4(1,1,1,1);\n"
    "        if (Flags & FLAG_HAS_SPECULAR) o.specular = input.specular.bgra;\n"
    "        return o;\n"
    "    }\n"
    "\n"
    "    o.pos = mul(float4(input.pos.xyz, 1.0), WorldViewProj);\n"
    "    float4 viewPos4 = mul(float4(input.pos.xyz, 1.0), WorldView);\n"
    "    o.viewpos = viewPos4;\n"
    "    float3 posCam = viewPos4.xyz;\n"
    "    float3 nrmCam = float3(0, 0, 1);\n"
    "\n"
    "    // Vertex colors\n"
    "    float4 vertDiffuse = (Flags & FLAG_HAS_DIFFUSE) ? input.diffuse.bgra : float4(1,1,1,1);\n"
    "    float4 vertSpecular = float4(0,0,0,0);\n"
    "    if (Flags & FLAG_HAS_SPECULAR) vertSpecular = input.specular.bgra;\n"
    "\n"
    "    // World-space normal (lighting) and camera-space normal (TCI / fog)\n"
    "    float3 worldNormal = float3(0, 0, 1);\n"
    "    if (Flags & FLAG_HAS_NORMAL) {\n"
    "        worldNormal = normalize(mul(input.normal, (float3x3)WorldInvTranspose));\n"
    "        nrmCam = normalize(mul(input.normal, (float3x3)WorldViewInvTranspose));\n"
    "    }\n"
    "\n"
    "    // Texture coordinates: per-stage TEXCOORDINDEX + texture matrix\n"
    "    o.tex0 = gen_tex(0, input, posCam, nrmCam).xyz;\n"
    "    o.tex1 = gen_tex(1, input, posCam, nrmCam).xyz;\n"
    "    o.tex2 = gen_tex(2, input, posCam, nrmCam).xyz;\n"
    "    o.tex3 = gen_tex(3, input, posCam, nrmCam).xyz;\n"
    "\n"
    "    // Lighting\n"
    "    if ((Flags & FLAG_LIGHTING) && (Flags & FLAG_HAS_NORMAL)) {\n"
    "        float3 worldPos = mul(float4(input.pos.xyz, 1.0), World).xyz;\n"
    "        float3 viewDir = normalize(EyePos.xyz - worldPos);\n"
    "\n"
    "        float4 litDiffuse = MatEmissive + MatAmbient * GlobalAmbient;\n"
    "        float4 litSpecular = float4(0,0,0,0);\n"
    "\n"
    "        for (uint i = 0; i < NumLights && i < 8u; i++) {\n"
    "            float3 lightDir;\n"
    "            float atten = 1.0;\n"
    "            float spotFactor = 1.0;\n"
    "            uint ltype = (uint)Lights[i].PosType.w;\n"
    "\n"
    "            if (ltype == 3u) {\n"  /* directional */
    "                lightDir = -normalize(Lights[i].DirRange.xyz);\n"
    "            } else {\n"  /* point or spot */
    "                float3 toLight = Lights[i].PosType.xyz - worldPos;\n"
    "                float dist = length(toLight);\n"
    "                if (dist > Lights[i].DirRange.w && Lights[i].DirRange.w > 0) continue;\n"
    "                lightDir = toLight / max(dist, 0.0001);\n"
    "                atten = 1.0 / (Lights[i].Atten.x + Lights[i].Atten.y * dist + Lights[i].Atten.z * dist * dist);\n"
    "\n"
    "                if (ltype == 2u) {\n"  /* spot */
    "                    float cosAngle = dot(-lightDir, normalize(Lights[i].DirRange.xyz));\n"
    "                    float cosOuter = Lights[i].Spot.y;\n"
    "                    float cosInner = Lights[i].Spot.x;\n"
    "                    spotFactor = saturate((cosAngle - cosOuter) / max(cosInner - cosOuter, 0.0001));\n"
    "                    spotFactor = pow(spotFactor, Lights[i].Atten.w);\n"
    "                }\n"
    "            }\n"
    "\n"
    "            float NdotL = max(dot(worldNormal, lightDir), 0.0);\n"
    "            litDiffuse += atten * spotFactor * (Lights[i].Ambient * MatAmbient + NdotL * Lights[i].Diffuse * MatDiffuse);\n"
    "\n"
    "            if (NdotL > 0.0 && MatPower > 0.0) {\n"
    "                float3 halfVec = normalize(lightDir + viewDir);\n"
    "                float NdotH = max(dot(worldNormal, halfVec), 0.0);\n"
    "                litSpecular += atten * spotFactor * pow(NdotH, MatPower) * Lights[i].Specular * MatSpecular;\n"
    "            }\n"
    "        }\n"
    "\n"
    "        o.diffuse = saturate(litDiffuse);\n"
    "        o.diffuse.a = MatDiffuse.a;\n"
    "        o.specular = saturate(litSpecular);\n"
    "    } else {\n"
    "        o.diffuse = vertDiffuse;\n"
    "        o.specular = vertSpecular;\n"
    "    }\n"
    "\n"
    "    // Vertex fog (FOGVERTEXMODE)\n"
    "    if ((uint)FogParams.w != 0u) {\n"
    "        float3 worldPos = mul(float4(input.pos.xyz, 1.0), World).xyz;\n"
    "        float fogDist = length(EyePos.xyz - worldPos);\n"
    "        o.fog = compute_fog(fogDist);\n"
    "    }\n"
    "\n"
    "    return o;\n"
    "}\n";

/* ================================================================
 * HLSL pixel shader (embedded)
 *
 * Supports:
 * - Up to 4 texture stages with D3D8 texture operations
 * - Per-stage COLOROP/COLORARG1/COLORARG2/ALPHAOP/ALPHAARG1/ALPHAARG2
 * - Fog blending
 * - Alpha test
 * ================================================================ */

static const char g_ps_body[] =
    "cbuffer PixelCB : register(b0) {\n"
    "    float4 TexFactor;\n"
    "    float4 FogColor;\n"
    "    float4 FogParams;\n"      /* x=start, y=end, z=density, w=table fog mode (D3DFOGMODE) */
    "    float  AlphaRef;\n"
    "    uint   AlphaFunc;\n"
    "    uint   PSFlags;\n"        /* bit 0: alpha test, bit 1: vertex fog, bit 2: specular add, bit 3: table fog, bit 4: range fog */
    "    uint   _pad0;\n"
    "    // Per-stage: x=colorop, y=colorarg1, z=colorarg2, w=alphaop\n"
    "    uint4  StageColor[4];\n"
    "    // Per-stage: x=alphaarg1, y=alphaarg2, z=0, w=0\n"
    "    uint4  StageAlpha[4];\n"
    "};\n"
    "\n"
    "struct PS_IN {\n"
    "    float4 pos      : SV_POSITION;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float3 tex0     : TEXCOORD0;\n"
    "    float3 tex1     : TEXCOORD1;\n"
    "    float3 tex2     : TEXCOORD2;\n"
    "    float3 tex3     : TEXCOORD3;\n"
    "    float  fog      : TEXCOORD4;\n"
    "    float4 viewpos  : TEXCOORD5;\n"
    "};\n"
    "\n"
    "// D3DFOGMODE: 0=NONE, 1=EXP, 2=EXP2, 3=LINEAR\n"
    "float compute_pfog(float dist) {\n"
    "    uint mode = (uint)FogParams.w;\n"
    "    if (mode == 1u) {\n"  /* EXP */
    "        return saturate(exp(-FogParams.z * dist));\n"
    "    } else if (mode == 2u) {\n"  /* EXP2 */
    "        float e = FogParams.z * dist;\n"
    "        return saturate(exp(-e * e));\n"
    "    } else if (mode == 3u) {\n"  /* LINEAR */
    "        return saturate((FogParams.y - dist) / max(FogParams.y - FogParams.x, 1e-6));\n"
    "    }\n"
    "    return 1.0;\n"
    "}\n"
    "\n"
    /* Resolve a texture argument value */
    "float4 resolve_arg(uint arg, float4 diffuse, float4 current,\n"
    "                   float4 texel, float4 tfactor, float4 specular) {\n"
    "    float4 val;\n"
    "    uint base = arg & 0x0Fu;\n"
    "    if      (base == 0u) val = diffuse;\n"   /* D3DTA_DIFFUSE */
    "    else if (base == 1u) val = current;\n"   /* D3DTA_CURRENT */
    "    else if (base == 2u) val = texel;\n"     /* D3DTA_TEXTURE */
    "    else if (base == 3u) val = tfactor;\n"   /* D3DTA_TFACTOR */
    "    else if (base == 4u) val = specular;\n"  /* D3DTA_SPECULAR */
    "    else val = current;\n"
    "    if (arg & 0x10u) val = 1.0 - val;\n"     /* D3DTA_COMPLEMENT */
    "    if (arg & 0x20u) val = val.aaaa;\n"      /* D3DTA_ALPHAREPLICATE */
    "    return val;\n"
    "}\n"
    "\n"
    /* Apply a D3D8 texture blend operation */
    "float apply_op(uint op, float a1, float a2, float diffuse_a, float tex_a, float cur_a, float factor_a) {\n"
    "    if (op <= 1u) return a1;\n"                /* DISABLE - shouldn't reach here, but fallback */
    "    if (op == 2u) return a1;\n"                /* SELECTARG1 */
    "    if (op == 3u) return a2;\n"                /* SELECTARG2 */
    "    if (op == 4u) return a1 * a2;\n"           /* MODULATE */
    "    if (op == 5u) return saturate(a1 * a2 * 2.0);\n"  /* MODULATE2X */
    "    if (op == 6u) return saturate(a1 * a2 * 4.0);\n"  /* MODULATE4X */
    "    if (op == 7u) return saturate(a1 + a2);\n"         /* ADD */
    "    if (op == 8u) return saturate(a1 + a2 - 0.5);\n"  /* ADDSIGNED */
    "    if (op == 9u) return saturate((a1 + a2 - 0.5) * 2.0);\n" /* ADDSIGNED2X */
    "    if (op == 10u) return saturate(a1 - a2);\n"         /* SUBTRACT */
    "    if (op == 11u) return saturate(a1 + a2 - a1 * a2);\n" /* ADDSMOOTH */
    "    if (op == 12u) return lerp(a2, a1, diffuse_a);\n"   /* BLENDDIFFUSEALPHA */
    "    if (op == 13u) return lerp(a2, a1, tex_a);\n"       /* BLENDTEXTUREALPHA */
    "    if (op == 14u) return lerp(a2, a1, factor_a);\n"    /* BLENDFACTORALPHA */
    "    if (op == 15u) return lerp(a2, a1, cur_a);\n"       /* BLENDCURRENTALPHA */
    "    if (op == 24u) {\n"                                  /* DOTPRODUCT3 */
    "        float d = saturate(4.0 * ((a1 - 0.5) * (a2 - 0.5)));\n"
    "        return d;\n"
    "    }\n"
    "    return a1 * a2;\n"  /* fallback = MODULATE */
    "}\n"
    "\n"
    "float4 main(PS_IN input) : SV_TARGET {\n"
    "    float4 current = input.diffuse;\n"
    "    float4 texels[4];\n"
    "\n"
    "    // Pre-sample all textures\n";

static const char g_ps_tail[] =
    "\n"
    "    // Process up to 4 texture stages\n"
    "    [unroll] for (uint i = 0; i < 4; i++) {\n"
    "        uint colorop  = StageColor[i].x;\n"
    "        if (colorop <= 1u) break;\n"  /* D3DTOP_DISABLE = stage off, stop */
    "\n"
    "        uint colorarg1 = StageColor[i].y;\n"
    "        uint colorarg2 = StageColor[i].z;\n"
    "        uint alphaop   = StageColor[i].w;\n"
    "        uint alphaarg1 = StageAlpha[i].x;\n"
    "        uint alphaarg2 = StageAlpha[i].y;\n"
    "\n"
    "        float4 texel = texels[i];\n"
    "\n"
    "        // Resolve color arguments\n"
    "        float4 carg1 = resolve_arg(colorarg1, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "        float4 carg2 = resolve_arg(colorarg2, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "\n"
    "        // Apply color operation per channel\n"
    "        float3 color;\n"
    "        if (colorop == 24u) {\n"
    "            // DOTPRODUCT3: dot of (arg1-0.5)*(arg2-0.5)*4, replicated\n"
    "            float d = saturate(4.0 * dot(carg1.rgb - 0.5, carg2.rgb - 0.5));\n"
    "            color = float3(d, d, d);\n"
    "        } else {\n"
    "            color.r = apply_op(colorop, carg1.r, carg2.r, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "            color.g = apply_op(colorop, carg1.g, carg2.g, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "            color.b = apply_op(colorop, carg1.b, carg2.b, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "        }\n"
    "\n"
    "        // Resolve alpha arguments\n"
    "        float4 aarg1 = resolve_arg(alphaarg1, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "        float4 aarg2 = resolve_arg(alphaarg2, input.diffuse, current, texel, TexFactor, input.specular);\n"
    "\n"
    "        // Apply alpha operation\n"
    "        float alpha;\n"
    "        if (alphaop <= 1u)\n"
    "            alpha = current.a;\n"  /* DISABLE on alpha = keep current */
    "        else\n"
    "            alpha = apply_op(alphaop, aarg1.a, aarg2.a, input.diffuse.a, texel.a, current.a, TexFactor.a);\n"
    "\n"
    "        current = float4(color, alpha);\n"
    "    }\n"
    "\n"
    "    // Add specular (D3DRS_SPECULARENABLE)\n"
    "    if (PSFlags & 4u)\n"
    "        current.rgb = saturate(current.rgb + input.specular.rgb);\n"
    "\n"
    "    // Fog blending\n"
    "    if (PSFlags & 2u)\n"  /* vertex fog: input.fog is the fog factor */
    "        current.rgb = lerp(FogColor.rgb, current.rgb, input.fog);\n"
    "\n"
    "    if (PSFlags & 8u) {\n"  /* table fog: compute fog per-pixel from view-space depth */
    "        float dist = (PSFlags & 16u) ? length(input.viewpos.xyz) : -input.viewpos.z;\n"
    "        float f = compute_pfog(dist);\n"
    "        current.rgb = lerp(FogColor.rgb, current.rgb, f);\n"
    "    }\n"
    "\n"
    "    // Alpha test\n"
    "    if (PSFlags & 1u) {\n"
    "        bool alphaOk = true;\n"
    "        if      (AlphaFunc == 1u) alphaOk = false;\n"
    "        else if (AlphaFunc == 2u) alphaOk = (current.a < AlphaRef);\n"
    "        else if (AlphaFunc == 3u) alphaOk = (current.a == AlphaRef);\n"
    "        else if (AlphaFunc == 4u) alphaOk = (current.a <= AlphaRef);\n"
    "        else if (AlphaFunc == 5u) alphaOk = (current.a > AlphaRef);\n"
    "        else if (AlphaFunc == 6u) alphaOk = (current.a != AlphaRef);\n"
    "        else if (AlphaFunc == 7u) alphaOk = (current.a >= AlphaRef);\n"
    "        if (!alphaOk) discard;\n"
    "    }\n"
    "\n"
    "    return current;\n"
    "}\n";

/* Build the fixed-function pixel shader source for a texture signature.
 *
 * The signature packs 2 bits per stage (stages 0..3, low bits first):
 *   0 = 2D texture   (Texture2D,  float2 sample coord)
 *   1 = cube texture (TextureCUBE, float3 sample coord)
 *   2 = 3D texture   (Texture3D,  float3 sample coord)
 * Texture object declarations must match the dimension of the SRV bound
 * at each stage, otherwise D3D11 fails to sample it correctly.
 */
#define FF_PS_SRC_SIZE 16384

static void build_ps_source(UINT sig, char *buf, int bufsize)
{
    int off = 0;
    int i;

    for (i = 0; i < 4; i++) {
        UINT dim = (sig >> (i * 2)) & 3u;
        const char *decl = (dim == 1u) ? "TextureCube" :
                           (dim == 2u) ? "Texture3D" : "Texture2D";
        off += snprintf(buf + off, bufsize - off, "%s tex%d : register(t%d);\n",
                        decl, i, i);
        off += snprintf(buf + off, bufsize - off,
                        "SamplerState samp%d : register(s%d);\n", i, i);
    }
    off += snprintf(buf + off, bufsize - off, "%s", g_ps_body);

    for (i = 0; i < 4; i++) {
        UINT dim = (sig >> (i * 2)) & 3u;
        off += snprintf(buf + off, bufsize - off,
                        "    texels[%d] = tex%d.Sample(samp%d, input.tex%d.%s);\n",
                        i, i, i, i, dim ? "xyz" : "xy");
    }
    off += snprintf(buf + off, bufsize - off, "%s", g_ps_tail);
}

/* ================================================================
 * Compiled shader objects
 * ================================================================ */

static ID3D11VertexShader  *g_vs = NULL;
static ID3DBlob            *g_vs_blob = NULL;
static ID3D11Buffer        *g_vs_cb = NULL;      /* VS transform CB (b0) */
static ID3D11Buffer        *g_vs_light_cb = NULL; /* VS lighting CB (b1) */
static ID3D11Buffer        *g_ps_cb = NULL;       /* PS constant buffer */

/* Fixed-function PS cache, keyed by per-stage texture signature.
 * The texture object declarations must match the dimensions of the SRVs
 * bound at each stage, so the PS is recompiled when the set of
 * 2D/cube/volume textures changes. Practically only a handful of
 * signatures ever occur. */
#define FF_PS_CACHE_SIZE 16

typedef struct {
    UINT               sig;
    ID3D11PixelShader *ps;
} FfPsCacheEntry;

static FfPsCacheEntry g_ff_ps_cache[FF_PS_CACHE_SIZE];
static int g_ff_ps_cache_count = 0;

/* Get (compiling if needed) the fixed-function pixel shader for a texture
 * signature. Returns NULL and logs on compile failure. */
static ID3D11PixelShader *ff_ps_get_shader(UINT sig)
{
    ID3DBlob *blob = NULL, *errors = NULL;
    ID3D11PixelShader *ps;
    char *src;
    HRESULT hr;
    int i;

    for (i = 0; i < g_ff_ps_cache_count; i++) {
        if (g_ff_ps_cache[i].sig == sig)
            return g_ff_ps_cache[i].ps;
    }

    src = (char *)malloc(FF_PS_SRC_SIZE);
    if (!src) return NULL;
    build_ps_source(sig, src, FF_PS_SRC_SIZE);

    hr = D3DCompile(src, strlen(src), "ps_ffp", NULL, NULL,
                    "main", "ps_5_0", 0, 0, &blob, &errors);
    free(src);

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: PS compile failed (sig 0x%02X): %s\n", sig,
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        if (errors) ID3D10Blob_Release(errors);
        return NULL;
    }

    hr = ID3D11Device_CreatePixelShader(d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(blob),
        ID3D10Blob_GetBufferSize(blob),
        NULL, &ps);
    ID3D10Blob_Release(blob);
    if (FAILED(hr)) return NULL;

    if (g_ff_ps_cache_count >= FF_PS_CACHE_SIZE) {
        /* Evict the oldest entry */
        ID3D11PixelShader_Release(g_ff_ps_cache[0].ps);
        memmove(&g_ff_ps_cache[0], &g_ff_ps_cache[1],
                (FF_PS_CACHE_SIZE - 1) * sizeof(FfPsCacheEntry));
        g_ff_ps_cache_count--;
    }
    g_ff_ps_cache[g_ff_ps_cache_count].sig = sig;
    g_ff_ps_cache[g_ff_ps_cache_count].ps = ps;
    g_ff_ps_cache_count++;

    return ps;
}

/* Compute the per-stage texture signature from the textures currently
 * bound to the pixel shader stages. */
static UINT ff_ps_compute_signature(void)
{
    UINT sig = 0;
    int i;

    for (i = 0; i < 4; i++) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(i);
        UINT dim = 0;
        if (tex) {
            D3DRESOURCETYPE type = IDirect3DBaseTexture8_GetType(tex);
            if (type == D3DRTYPE_CUBETEXTURE)      dim = 1;
            else if (type == D3DRTYPE_VOLUMETEXTURE) dim = 2;
        }
        sig |= dim << (i * 2);
    }
    return sig;
}

/* VS transform constant buffer layout (must match HLSL TransformCB) */
typedef struct {
    float wvp[16];               /* WorldViewProj (column-major) */
    float world[16];             /* World matrix (column-major) */
    float world_inv_transpose[16]; /* World inverse transpose (column-major) */
    float screen_w, screen_h;
    UINT  flags;
    float _pad0;
    float eye_pos[4];
    float fog_params[4];         /* start, end, density, mode */
    float world_view[16];        /* World*View, camera space (column-major) */
    float world_view_inv_transpose[16]; /* inverse-transpose(World*View) */
    float tex_mat[4][16];        /* per-stage texture matrices (column-major) */
    UINT  texcoord_index[4];     /* per-stage D3DTSS_TEXCOORDINDEX */
} VSTransformConstants;

/* VS lighting constant buffer layout (must match HLSL LightingCB) */
typedef struct {
    float global_ambient[4];
    float mat_diffuse[4];
    float mat_ambient[4];
    float mat_specular[4];
    float mat_emissive[4];
    float mat_power;
    UINT  num_lights;
    float _lpad0[2];
    struct {
        float pos_type[4];       /* xyz=position, w=type */
        float dir_range[4];      /* xyz=direction, w=range */
        float diffuse[4];
        float ambient[4];
        float specular[4];
        float atten[4];          /* const, linear, quad, falloff */
        float spot[4];           /* cos(theta/2), cos(phi/2), 0, 0 */
    } lights[8];
} VSLightingConstants;

/* PS constant buffer layout (must match HLSL PixelCB) */
typedef struct {
    float tex_factor[4];
    float fog_color[4];
    float fog_params[4];         /* start, end, density, table fog mode (D3DFOGMODE) */
    float alpha_ref;
    UINT  alpha_func;
    UINT  ps_flags;              /* bit 0: alpha test, bit 1: vertex fog, bit 2: specular add, bit 3: table fog, bit 4: range fog */
    UINT  _pad0;
    UINT  stage_color[4][4];     /* [stage][x=colorop, y=arg1, z=arg2, w=alphaop] */
    UINT  stage_alpha[4][4];     /* [stage][x=alphaarg1, y=alphaarg2, z=0, w=0] */
} PSConstants;

/* ================================================================
 * Input layout cache (FVF → ID3D11InputLayout)
 * ================================================================ */

#define MAX_LAYOUT_CACHE 32

typedef struct {
    DWORD               fvf;
    ID3D11InputLayout  *layout;
} LayoutCacheEntry;

static LayoutCacheEntry g_layout_cache[MAX_LAYOUT_CACHE];
static int g_layout_cache_count = 0;

/* Number of floats in texcoord set `t` (0-3) according to the FVF.
 * Each set uses 2 bits at bit position (16 + t*2); 0 means default (2). */
static UINT fvf_texcoord_size(DWORD fvf, UINT t)
{
    DWORD field = (fvf >> (16 + t * 2)) & 0x3;
    return field == 0 ? 2 : field;
}

/*
 * Build input layout for a given FVF.
 *
 * The vertex shader declares all 8 inputs. For FVF components that are
 * missing, we add dummy elements at offset 0 — the shader ignores them
 * via the Flags constant buffer.
 */
static ID3D11InputLayout *get_or_create_layout(DWORD fvf)
{
    D3D11_INPUT_ELEMENT_DESC elems[16];
    UINT elem_count = 0;
    UINT offset = 0;
    HRESULT hr;
    int i;

    for (i = 0; i < g_layout_cache_count; i++) {
        if (g_layout_cache[i].fvf == fvf)
            return g_layout_cache[i].layout;
    }

    /* POSITION */
    if (d3d8_fvf_transformed(fvf)) {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++;
    } else {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++;
    }
    offset = d3d8_fvf_position_bytes(fvf);

    /* NORMAL */
    if (fvf & D3DFVF_NORMAL) {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++; offset += 12;
    } else {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++;
    }

    /* COLOR0 (Diffuse) */
    if (fvf & D3DFVF_DIFFUSE) {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++; offset += 4;
    } else {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++;
    }

    /* COLOR1 (Specular) */
    if (fvf & D3DFVF_SPECULAR) {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++; offset += 4;
    } else {
        elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
        elem_count++;
    }

    /* TEXCOORD0-3 */
    {
        UINT tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
        static const DXGI_FORMAT tc_fmt[5] = {
            DXGI_FORMAT_R32G32_FLOAT,      /* 2 (default) */
            DXGI_FORMAT_R32_FLOAT,         /* 1 */
            DXGI_FORMAT_R32G32_FLOAT,      /* 2 */
            DXGI_FORMAT_R32G32B32_FLOAT,   /* 3 */
            DXGI_FORMAT_R32G32B32A32_FLOAT /* 4 */
        };
        UINT t;
        for (t = 0; t < 4; t++) {
            if (t < tex_count) {
                UINT size = fvf_texcoord_size(fvf, t);
                DXGI_FORMAT f = tc_fmt[size];
                elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"TEXCOORD", t, f, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
                elem_count++; offset += size * 4;
            } else {
                elems[elem_count] = (D3D11_INPUT_ELEMENT_DESC){"TEXCOORD", t, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
                elem_count++;
            }
        }
    }

    {
        ID3D11InputLayout *layout = NULL;
        hr = ID3D11Device_CreateInputLayout(
            d3d8_GetD3D11Device(),
            elems, elem_count,
            ID3D10Blob_GetBufferPointer(g_vs_blob),
            ID3D10Blob_GetBufferSize(g_vs_blob),
            &layout);

        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: CreateInputLayout failed for FVF 0x%lX: 0x%08lX\n", fvf, hr);
            layout = NULL;
        }

        if (g_layout_cache_count < MAX_LAYOUT_CACHE) {
            g_layout_cache[g_layout_cache_count].fvf = fvf;
            g_layout_cache[g_layout_cache_count].layout = layout;
            g_layout_cache_count++;
        }

        return layout;
    }
}

/* ================================================================
 * Matrix math helpers
 * ================================================================ */

static void mat4_mul(float *result, const float *a, const float *b)
{
    int i, j, k;
    float tmp[16];
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++) {
            tmp[i * 4 + j] = 0;
            for (k = 0; k < 4; k++)
                tmp[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
        }
    memcpy(result, tmp, sizeof(tmp));
}

static void mat4_transpose(float *result, const float *src)
{
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            result[j * 4 + i] = src[i * 4 + j];
}

/* Compute inverse of a 4x4 matrix (for normal transform).
 * Uses cofactor expansion. Falls back to transpose if singular. */
static void mat4_inverse(float *out, const float *m)
{
    float inv[16], det;
    int i;

    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15]
              + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15]
              - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15]
              + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14]
              - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15]
              - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15]
              + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15]
              - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14]
              + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15]
              + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15]
              - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15]
              + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14]
              - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11]
              - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11]
              + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11]
              - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10]
              + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-12f) {
        /* Singular — use transpose as fallback */
        mat4_transpose(out, m);
        return;
    }

    det = 1.0f / det;
    for (i = 0; i < 16; i++)
        out[i] = inv[i] * det;
}

/* ================================================================
 * Initialization / Shutdown
 * ================================================================ */

HRESULT d3d8_shaders_init(void)
{
    ID3DBlob *errors = NULL;
    D3D11_BUFFER_DESC cbd;
    HRESULT hr;

    /* Compile vertex shader */
    hr = D3DCompile(g_vs_source, strlen(g_vs_source), "vs_ffp",
                    NULL, NULL, "main", "vs_5_0", 0, 0, &g_vs_blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VS compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "unknown");
        if (errors) ID3D10Blob_Release(errors);
        return hr;
    }

    hr = ID3D11Device_CreateVertexShader(d3d8_GetD3D11Device(),
        ID3D10Blob_GetBufferPointer(g_vs_blob),
        ID3D10Blob_GetBufferSize(g_vs_blob),
        NULL, &g_vs);
    if (FAILED(hr)) return hr;

    /* Compile the default (all-2D) pixel shader */
    g_ff_ps_cache_count = 0;
    memset(g_ff_ps_cache, 0, sizeof(g_ff_ps_cache));
    if (!ff_ps_get_shader(0))
        return E_FAIL;

    /* Create VS transform constant buffer (b0) */
    memset(&cbd, 0, sizeof(cbd));
    cbd.ByteWidth = (sizeof(VSTransformConstants) + 15) & ~15;
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vs_cb);
    if (FAILED(hr)) return hr;

    /* Create VS lighting constant buffer (b1) */
    cbd.ByteWidth = (sizeof(VSLightingConstants) + 15) & ~15;
    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_vs_light_cb);
    if (FAILED(hr)) return hr;

    /* Create PS constant buffer */
    cbd.ByteWidth = (sizeof(PSConstants) + 15) & ~15;
    hr = ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &cbd, NULL, &g_ps_cb);
    if (FAILED(hr)) return hr;

    fprintf(stderr, "D3D8: Fixed-function shaders compiled OK (multi-texture + lighting + fog)\n");
    return S_OK;
}

void d3d8_shaders_shutdown(void)
{
    int i;
    for (i = 0; i < g_layout_cache_count; i++) {
        if (g_layout_cache[i].layout)
            ID3D11InputLayout_Release(g_layout_cache[i].layout);
    }
    g_layout_cache_count = 0;

    for (i = 0; i < g_ff_ps_cache_count; i++) {
        if (g_ff_ps_cache[i].ps)
            ID3D11PixelShader_Release(g_ff_ps_cache[i].ps);
    }
    g_ff_ps_cache_count = 0;

    if (g_ps_cb)       { ID3D11Buffer_Release(g_ps_cb); g_ps_cb = NULL; }
    if (g_vs_light_cb) { ID3D11Buffer_Release(g_vs_light_cb); g_vs_light_cb = NULL; }
    if (g_vs_cb)       { ID3D11Buffer_Release(g_vs_cb); g_vs_cb = NULL; }
    if (g_vs)          { ID3D11VertexShader_Release(g_vs); g_vs = NULL; }
    if (g_vs_blob)     { ID3D10Blob_Release(g_vs_blob); g_vs_blob = NULL; }
}

/* ================================================================
 * Pre-draw binding
 * ================================================================ */

void d3d8_shaders_prepare_draw(DWORD fvf)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11InputLayout *layout;
    ID3D11PixelShader *ps;
    D3D11_MAPPED_SUBRESOURCE mapped;
    const D3DMATRIX *world, *view, *proj;
    const DWORD *rs;
    HRESULT hr;
    UINT tex_count;

    if (!ctx || !g_vs) return;

    rs = d3d8_GetRenderStates();
    tex_count = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

    /* Bind the fixed-function pixel shader matching the texture types
     * currently bound to each stage (2D/cube/volume). */
    ps = ff_ps_get_shader(ff_ps_compute_signature());
    if (!ps) return;

    /* Bind shaders */
    ID3D11DeviceContext_VSSetShader(ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);

    /* Bind input layout */
    layout = get_or_create_layout(fvf);
    if (layout)
        ID3D11DeviceContext_IASetInputLayout(ctx, layout);

    /* ---- VS Transform CB (b0) ---- */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vs_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        VSTransformConstants *cb = (VSTransformConstants *)mapped.pData;
        UINT i;
        memset(cb, 0, sizeof(*cb));

        if (d3d8_fvf_transformed(fvf)) {
            float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
            memcpy(cb->wvp, identity, sizeof(identity));
            memcpy(cb->world, identity, sizeof(identity));
            memcpy(cb->world_inv_transpose, identity, sizeof(identity));
            memcpy(cb->world_view, identity, sizeof(identity));
            memcpy(cb->world_view_inv_transpose, identity, sizeof(identity));
            cb->screen_w = (float)d3d8_GetBackbufferWidth();
            cb->screen_h = (float)d3d8_GetBackbufferHeight();
            cb->flags = 0x01; /* pre-transformed */
        } else {
            float wv[16], wvp[16], wvp_t[16], world_t[16];
            float world_inv[16], world_inv_t[16];
            float wv_t[16], wv_inv[16], wv_inv_t[16];

            world = d3d8_GetTransform(D3DTS_WORLD);
            view  = d3d8_GetTransform(D3DTS_VIEW);
            proj  = d3d8_GetTransform(D3DTS_PROJECTION);

            mat4_mul(wv, (const float *)world, (const float *)view);
            mat4_mul(wvp, wv, (const float *)proj);
            mat4_transpose(wvp_t, wvp);
            memcpy(cb->wvp, wvp_t, sizeof(wvp_t));

            mat4_transpose(world_t, (const float *)world);
            memcpy(cb->world, world_t, sizeof(world_t));

            /* WorldInvTranspose = transpose(inverse(world)) */
            mat4_inverse(world_inv, (const float *)world);
            mat4_transpose(world_inv_t, world_inv);
            memcpy(cb->world_inv_transpose, world_inv_t, sizeof(world_inv_t));

            /* WorldView (camera space) and its inverse-transpose */
            mat4_transpose(wv_t, wv);
            memcpy(cb->world_view, wv_t, sizeof(wv_t));
            mat4_inverse(wv_inv, wv);
            mat4_transpose(wv_inv_t, wv_inv);
            memcpy(cb->world_view_inv_transpose, wv_inv_t, sizeof(wv_inv_t));

            cb->screen_w = (float)d3d8_GetBackbufferWidth();
            cb->screen_h = (float)d3d8_GetBackbufferHeight();
            cb->flags = 0;

            /* Compute eye position from inverse view matrix */
            {
                float view_inv[16];
                mat4_inverse(view_inv, (const float *)view);
                cb->eye_pos[0] = view_inv[12];
                cb->eye_pos[1] = view_inv[13];
                cb->eye_pos[2] = view_inv[14];
                cb->eye_pos[3] = 1.0f;
            }
        }

        if (fvf & D3DFVF_DIFFUSE) cb->flags |= 0x02;
        if (fvf & D3DFVF_SPECULAR) cb->flags |= 0x04;
        if (fvf & D3DFVF_NORMAL) cb->flags |= 0x08;
        if (rs && rs[D3DRS_LIGHTING]) cb->flags |= 0x10;

        /* Fog parameters */
        if (rs && rs[D3DRS_FOGENABLE]) {
            float fog_start, fog_end, fog_density;
            memcpy(&fog_start, &rs[D3DRS_FOGSTART], sizeof(float));
            memcpy(&fog_end, &rs[D3DRS_FOGEND], sizeof(float));
            memcpy(&fog_density, &rs[D3DRS_FOGDENSITY], sizeof(float));
            cb->fog_params[0] = fog_start;
            cb->fog_params[1] = fog_end;
            cb->fog_params[2] = fog_density;
            if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE) {
                /* Table fog is applied per-pixel in the PS; disable VS fog. */
                cb->fog_params[3] = (float)D3DFOG_NONE;
            } else {
                cb->fog_params[3] = (float)rs[D3DRS_FOGVERTEXMODE];
            }
        }

        /* Per-stage texture matrices and texcoord indices */
        for (i = 0; i < 4; i++) {
            const float *tm = (const float *)d3d8_GetTransform(D3DTS_TEXTURE0 + i);
            const DWORD *ts = d3d8_GetTSS(i);
            if (tm) {
                float tm_t[16];
                mat4_transpose(tm_t, tm);
                memcpy(cb->tex_mat[i], tm_t, sizeof(tm_t));
            } else {
                memset(cb->tex_mat[i], 0, sizeof(cb->tex_mat[i]));
                cb->tex_mat[i][0] = cb->tex_mat[i][5] = cb->tex_mat[i][10] = cb->tex_mat[i][15] = 1.0f;
            }
            cb->texcoord_index[i] = ts ? ts[D3DTSS_TEXCOORDINDEX] : (UINT)i;
        }

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vs_cb, 0);
    }

    /* ---- VS Lighting CB (b1) ---- */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_vs_light_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        VSLightingConstants *lb = (VSLightingConstants *)mapped.pData;
        const D3DMATERIAL8 *mat = d3d8_GetMaterial();
        UINT i, active_count = 0;
        memset(lb, 0, sizeof(*lb));

        /* Global ambient */
        if (rs) {
            DWORD amb = rs[D3DRS_AMBIENT];
            lb->global_ambient[0] = ((amb >> 16) & 0xFF) / 255.0f;
            lb->global_ambient[1] = ((amb >>  8) & 0xFF) / 255.0f;
            lb->global_ambient[2] = ((amb >>  0) & 0xFF) / 255.0f;
            lb->global_ambient[3] = ((amb >> 24) & 0xFF) / 255.0f;
        }

        /* Material */
        if (mat) {
            memcpy(lb->mat_diffuse,  &mat->Diffuse, 16);
            memcpy(lb->mat_ambient,  &mat->Ambient, 16);
            memcpy(lb->mat_specular, &mat->Specular, 16);
            memcpy(lb->mat_emissive, &mat->Emissive, 16);
            lb->mat_power = mat->Power;
        } else {
            lb->mat_diffuse[0] = lb->mat_diffuse[1] = lb->mat_diffuse[2] = lb->mat_diffuse[3] = 1.0f;
            lb->mat_ambient[0] = lb->mat_ambient[1] = lb->mat_ambient[2] = lb->mat_ambient[3] = 1.0f;
        }

        /* Lights — pack enabled lights contiguously */
        for (i = 0; i < d3d8_GetNumLights() && active_count < 8; i++) {
            const D3DLIGHT8 *light;
            if (!d3d8_GetLightEnable(i)) continue;
            light = d3d8_GetLight(i);
            if (!light) continue;

            lb->lights[active_count].pos_type[0] = light->Position.x;
            lb->lights[active_count].pos_type[1] = light->Position.y;
            lb->lights[active_count].pos_type[2] = light->Position.z;
            lb->lights[active_count].pos_type[3] = (float)light->Type;

            lb->lights[active_count].dir_range[0] = light->Direction.x;
            lb->lights[active_count].dir_range[1] = light->Direction.y;
            lb->lights[active_count].dir_range[2] = light->Direction.z;
            lb->lights[active_count].dir_range[3] = light->Range;

            memcpy(lb->lights[active_count].diffuse,  &light->Diffuse, 16);
            memcpy(lb->lights[active_count].ambient,  &light->Ambient, 16);
            memcpy(lb->lights[active_count].specular, &light->Specular, 16);

            lb->lights[active_count].atten[0] = light->Attenuation0;
            lb->lights[active_count].atten[1] = light->Attenuation1;
            lb->lights[active_count].atten[2] = light->Attenuation2;
            lb->lights[active_count].atten[3] = light->Falloff;

            lb->lights[active_count].spot[0] = cosf(light->Theta * 0.5f);
            lb->lights[active_count].spot[1] = cosf(light->Phi * 0.5f);

            active_count++;
        }
        lb->num_lights = active_count;

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_vs_light_cb, 0);
    }

    /* ---- PS Constant Buffer ---- */
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g_ps_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        PSConstants *pc = (PSConstants *)mapped.pData;
        UINT stage;
        memset(pc, 0, sizeof(*pc));

        /* Texture factor */
        if (rs) {
            DWORD tf = rs[D3DRS_TEXTUREFACTOR];
            pc->tex_factor[0] = ((tf >> 16) & 0xFF) / 255.0f;
            pc->tex_factor[1] = ((tf >>  8) & 0xFF) / 255.0f;
            pc->tex_factor[2] = ((tf >>  0) & 0xFF) / 255.0f;
            pc->tex_factor[3] = ((tf >> 24) & 0xFF) / 255.0f;
        }

        /* Fog color */
        if (rs && rs[D3DRS_FOGENABLE]) {
            DWORD fc = rs[D3DRS_FOGCOLOR];
            pc->fog_color[0] = ((fc >> 16) & 0xFF) / 255.0f;
            pc->fog_color[1] = ((fc >>  8) & 0xFF) / 255.0f;
            pc->fog_color[2] = ((fc >>  0) & 0xFF) / 255.0f;
            pc->fog_color[3] = 1.0f;

            if (rs[D3DRS_FOGTABLEMODE] != D3DFOG_NONE) {
                /* Table fog: compute the factor per-pixel from view-space depth */
                float fog_start, fog_end, fog_density;
                memcpy(&fog_start, &rs[D3DRS_FOGSTART], sizeof(float));
                memcpy(&fog_end, &rs[D3DRS_FOGEND], sizeof(float));
                memcpy(&fog_density, &rs[D3DRS_FOGDENSITY], sizeof(float));
                pc->fog_params[0] = fog_start;
                pc->fog_params[1] = fog_end;
                pc->fog_params[2] = fog_density;
                pc->fog_params[3] = (float)rs[D3DRS_FOGTABLEMODE];
                pc->ps_flags |= 8; /* table fog */
                if (rs[D3DRS_RANGEFOGENABLE]) pc->ps_flags |= 16; /* range fog */
            } else {
                pc->ps_flags |= 2; /* vertex fog (factor computed in the VS) */
            }
        }

        /* Alpha test */
        if (rs && rs[D3DRS_ALPHATESTENABLE]) {
            pc->alpha_ref = rs[D3DRS_ALPHAREF] / 255.0f;
            pc->alpha_func = rs[D3DRS_ALPHAFUNC];
            pc->ps_flags |= 1;
        }

        /* Specular add */
        if (rs && rs[D3DRS_SPECULARENABLE])
            pc->ps_flags |= 4;

        /* Per-stage texture state */
        for (stage = 0; stage < 4; stage++) {
            const DWORD *tss = d3d8_GetTSS(stage);
            if (!tss) {
                pc->stage_color[stage][0] = D3DTOP_DISABLE;
                continue;
            }

            DWORD colorop = tss[D3DTSS_COLOROP];
            if (colorop == 0) colorop = (stage == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;

            pc->stage_color[stage][0] = colorop;
            pc->stage_color[stage][1] = tss[D3DTSS_COLORARG1] ? tss[D3DTSS_COLORARG1] : D3DTA_TEXTURE;
            pc->stage_color[stage][2] = tss[D3DTSS_COLORARG2] ? tss[D3DTSS_COLORARG2] : D3DTA_CURRENT;
            pc->stage_color[stage][3] = tss[D3DTSS_ALPHAOP] ? tss[D3DTSS_ALPHAOP] :
                                         (stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE);

            pc->stage_alpha[stage][0] = tss[D3DTSS_ALPHAARG1] ? tss[D3DTSS_ALPHAARG1] : D3DTA_TEXTURE;
            pc->stage_alpha[stage][1] = tss[D3DTSS_ALPHAARG2] ? tss[D3DTSS_ALPHAARG2] : D3DTA_CURRENT;
        }

        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_ps_cb, 0);
    }

    /* Bind constant buffers */
    {
        ID3D11Buffer *vs_cbs[2] = { g_vs_cb, g_vs_light_cb };
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, vs_cbs);
    }
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 1, &g_ps_cb);
}
