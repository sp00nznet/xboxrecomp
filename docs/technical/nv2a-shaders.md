# NV2A Shader Translation

Xbox games do not ship HLSL. They configure the NV2A's fixed-function combiner
pipeline, or upload raw vertex-shader microcode. Both have to become something
D3D11 or OpenGL will accept, and both are translated at runtime and cached.

This document covers the two translators. For what is and is not implemented,
see [gap-analysis.md](gap-analysis.md); for the wider D3D8 layer, see
[d3d-translation.md](d3d-translation.md).

## Register combiners → HLSL pixel shaders

Xbox games do not use traditional pixel shaders. They configure the NV2A's
8-stage register combiner pipeline. Each stage performs independent RGB and
alpha math — multiply, dot product, MUX — over a register file of textures,
vertex colours and constants. A final combiner blends the result.

The game sets this up as a packed descriptor:

```c
SetPixelShader(0x00000103);   /* 3 stages, tex0 = 2D, tex1 = 2D */
```

At draw time that configuration is translated into an HLSL pixel shader:

```
Stage 0: r0.rgb = tex0 * diffuse
Stage 1: r0.rgb = r0 * tex1          (environment-map modulate)
Stage 2: r0.a   = tex0.a * diffuse.a
Final:   output = r0
```

A 128-entry shader cache keys on the combiner configuration, so each unique
setup is compiled once and reused. Multi-texturing is covered outright.

Bump and environment mapping are **partial**: the combiner side is there, but
texture-coordinate generation (`TEXCOORDINDEX` with the camera-space modes) is
not, so effects that depend on generated coordinates will not look right yet.

## Vertex-shader microcode → HLSL vertex shaders

When a game uses programmable vertex shaders — water displacement, skeletal
animation, custom lighting — it uploads NV2A microcode rather than any
high-level source. Each instruction is 128 bits and carries a paired MAC and
ILU operation.

The translator parses that microcode and emits HLSL:

- **14 MAC ops** — `MOV`, `MUL`, `ADD`, `MAD`, `DP3`, `DP4`, `DPH`, `DST`,
  `MIN`, `MAX`, `SLT`, `SGE`, `ARL`
- **8 ILU ops** — `MOV`, `RCP`, `RCC`, `RSQ`, `EXP`, `LOG`, `LIT`
- **192 constant registers**, 12 temporaries, 16 vertex inputs
- Relative addressing through the address register (`A0`)
- A 64-entry compiled-shader cache

Because both halves of an instruction issue together, the translator has to
emit them so that the MAC and ILU results are written from the *pre-instruction*
register values — reading a register the paired op just wrote is the classic
way to get this subtly wrong.

## Where the code lives

| Piece | File |
|---|---|
| Combiner → HLSL | `src/d3d/d3d8_combiners.c`, `src/d3d/d3d8_combiners.h` |
| Vertex microcode → HLSL | `src/nv2a/nv2a_pgraph_d3d11.c` |
| Texture unswizzling | `src/d3d/d3d8_swizzle.h` |
| Pushbuffer path: vertex microcode field table (one decoder) | `src/kernel/nv2a_vsh_fields.h` |
| Pushbuffer path: combiner field table and CPU evaluator (one decoder) | `src/kernel/nv2a_combiner.h`, `src/kernel/nv2a_combiner.c` |
| Pushbuffer path: parsers shared by both generators | `src/d3d/d3d8_vsh_parse.c`, `src/d3d/d3d8_combiners_parse.c` |
| Pushbuffer path: HLSL (D3D11) and MSL (Metal, macOS) emitters | `src/d3d/d3d8_vsh_hlsl.c`, `src/d3d/d3d8_combiners_hlsl.c`, `src/d3d/d3d8_vsh_msl.c`, `src/d3d/d3d8_combiners_msl.c` |
| Pushbuffer path: CPU vertex interpreter | `src/kernel/nv2a_vsh_cpu.c` |

`d3d8_combiners.h` and `d3d8_swizzle.h` cite xemu as a reference for the
hardware's behaviour; both are our own implementations. See [NOTICE](../../NOTICE).
