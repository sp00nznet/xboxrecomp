#version 450
// SPDX-License-Identifier: MIT
//
// Contrast Adaptive Sharpening, the sharpen-only path of AMD FidelityFX CAS
// (ffx_cas.h, CasFilter with noScaling, as built with CAS_BETTER_DIAGONALS
// and the green-channel weights), ported to a slang pass for xboxrecomp.
// https://github.com/GPUOpen-Effects/FidelityFX-CAS
//
// Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.
// Copyright (c) 2026 the xboxrecomp contributors (this port).
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#pragma name XBR_CAS

// Two changes from ffx_cas.h. CAS_AMOUNT mixes the result with the input:
// even at sharpness 0, full CAS raises the gradient energy of a 480-line HUD
// by 1.5 to 1.8 times, and the sharpness knob cannot go lower. At the default
// 0.4 the HUD stays within 1.3 times stock. And the input stays as the game
// drew it (gamma-encoded): CasInput is left empty, as in the FidelityFX
// sample's default.
#pragma parameter CAS_SHARPNESS "CAS sharpness" 0.0 0.0 1.0 0.05
#pragma parameter CAS_AMOUNT "CAS amount" 0.4 0.0 1.0 0.05

layout(push_constant) uniform Push {
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
    float CAS_SHARPNESS;
    float CAS_AMOUNT;
} params;

layout(std140, set = 0, binding = 0) uniform UBO {
    mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main() {
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;

vec3 tap(float x, float y) {
    return texture(Source, vTexCoord + vec2(x, y) * params.SourceSize.zw).rgb;
}

void main() {
    // a b c
    // d e f
    // g h i
    vec3 a = tap(-1.0, -1.0), b = tap(0.0, -1.0), c = tap(1.0, -1.0);
    vec3 d = tap(-1.0, 0.0), e = tap(0.0, 0.0), f = tap(1.0, 0.0);
    vec3 g = tap(-1.0, 1.0), h = tap(0.0, 1.0), i = tap(1.0, 1.0);

    // Soft min and max: the 5-tap cross plus the 3x3 box, so twice as big.
    vec3 mn = min(min(min(d, e), min(f, b)), h);
    vec3 mn2 = min(min(min(mn, a), min(c, g)), i);
    mn += mn2;
    vec3 mx = max(max(max(d, e), max(f, b)), h);
    vec3 mx2 = max(max(max(mx, a), max(c, g)), i);
    mx += mx2;

    // The distance to the signal limit over the max, shaped: 0 leaves a
    // high-contrast edge alone, 1 sharpens soft detail fully.
    float amp = sqrt(clamp(min(mn.g, 2.0 - mx.g) / max(mx.g, 1e-5), 0.0, 1.0));

    //  0 w 0
    //  w 1 w
    //  0 w 0
    // CasSetup's peak: a lobe of -1/8 at sharpness 0 to -1/5 at 1.
    float peak = -1.0 / mix(8.0, 5.0, clamp(params.CAS_SHARPNESS, 0.0, 1.0));
    float w = amp * peak;
    vec3 o = clamp((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
    FragColor = vec4(mix(e, o, params.CAS_AMOUNT), 1.0);
}
