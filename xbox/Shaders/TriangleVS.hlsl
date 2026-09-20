// SPDX-License-Identifier: GPL-2.0-or-later
float4 main(uint id : SV_VertexID) : SV_POSITION {
    float2 vertices[3] = {float2(0, 0.85), float2(0.85, -0.85), float2(-0.85, -0.85)};
    return float4(vertices[id], 0, 1);
}
