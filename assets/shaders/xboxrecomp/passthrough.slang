#version 450
// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the xboxrecomp contributors.
#pragma name XBR_Passthrough

// A copy of the input. smaa.slangp starts with it so SMAA_Input, the alias
// SMAA's last pass blends from, names this pass's output. Ours, in place of
// slang-shaders' stock.slang, which carries no licence.

layout(push_constant) uniform Push {
    vec4 SourceSize;
    vec4 OriginalSize;
    vec4 OutputSize;
    uint FrameCount;
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

void main() {
    FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0);
}
