#pragma once

// HLSL for the lens, compiled at startup with D3DCompile. The math must match app/LensMath.h.
inline constexpr char kLensShaderSource[] = R"hlsl(
cbuffer Lens : register(b0)
{
    float2 center;      // lens center, monitor pixels
    float2 viewOrigin;  // overlay top-left, monitor pixels
    float2 boxOrigin;   // top-left of the copied source box, monitor pixels
    float2 invTexSize;  // 1 / size of the texture holding the box
    float  radius;      // R
    float  strength;    // S
    float  gradual;     // 1: fade zoom to 1x at the edge, 0: fixed zoom S inside the lens
    float  pad;
};

Texture2D    src  : register(t0);
SamplerState samp : register(s0);

struct VSOut { float4 pos : SV_Position; };

// Fullscreen triangle from SV_VertexID; no vertex buffer needed.
VSOut VS(uint id : SV_VertexID)
{
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PS(VSOut i) : SV_Target
{
    float2 p = viewOrigin + i.pos.xy;  // pixel center, monitor pixels
    float2 d = p - center;
    float  r = length(d);
    if (r >= radius)
        return 0;

    float t = 1 - r / radius;             // 1 at center, 0 at edge
    float z = gradual > 0.5
        ? 1 + (strength - 1) * t * t      // S at center, gradually -> 1 at edge
        : strength;                       // fixed zoom across the whole lens
    float2 uv = (center + d / z - boxOrigin) * invTexSize;

    float3 c = src.SampleLevel(samp, uv, 0).rgb;
    float  a = saturate(radius - r);      // 1px anti-aliased edge
    return float4(c * a, a);              // premultiplied alpha
}
)hlsl";
