#pragma once

// CPU mirror of the pixel shader in render/LensShader.h. Keep both in sync;
// tests verify the properties documented in CLAUDE.md against this version.
namespace lens {

// Zoom factor at distance r from the center.
// gradual: S at r = 0, falling off quadratically in distance to the edge to exactly 1 at r >= R.
//   Zero slope at the edge (seamless) and no flat plateau at the center, which spreads the
//   unavoidable radial compression over the whole lens instead of a ring.
// fixed (gradual = false): S everywhere inside the lens, like a scope; hard edge at R.
inline float zoomAt(float r, float R, float S, bool gradual = true) {
    if (R <= 0.0f || S <= 1.0f || r >= R) return 1.0f;
    if (!gradual) return S;
    float t = 1.0f - r / R;  // 1 at center, 0 at edge
    return 1.0f + (S - 1.0f) * t * t;
}

// Distance from the center at which the source image is sampled.
inline float sourceRadius(float r, float R, float S, bool gradual = true) {
    return r / zoomAt(r, R, S, gradual);
}

}  // namespace lens
