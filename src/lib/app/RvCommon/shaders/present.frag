#version 440

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;

layout(binding = 1) uniform sampler2D tex;

// std140: mat4 @0, vec4 params @64
// params.x = scale factor
//   mode 1: multiply linear nits before re-PQ (SDR-white match)
//   mode 3: multiply after sRGB EOTF so encoded white (1.0) matches compositor SDR white
// params.y = mode:
//   0 = passthrough
//   1 = PQ → lin → ×s → PQ  (HDR10 + Wayland SDR-white hack)
//   2 = PQ → linear relative (1.0 = sdrWhite nits)  for p3linear/srgblinear from PQ buffers
//   3 = Display P3 Extended: sRGB EOTF → linear P3, then ×s (white match)
// params.z = sdrWhite nits (mode 2)
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;
};

const float m1 = 0.1593017578125;
const float m2 = 78.84375;
const float c1 = 0.8359375;
const float c2 = 18.8515625;
const float c3 = 18.6875;

float pqToLinear(float E)
{
    E = clamp(E, 0.0, 1.0);
    float Ep = pow(E, 1.0 / m2);
    float num = max(Ep - c1, 0.0);
    float den = c2 - c3 * Ep;
    if (den <= 0.0)
        return 0.0;
    return pow(num / den, 1.0 / m1);
}

float linearToPq(float Y)
{
    Y = max(Y, 0.0);
    float Ym = pow(Y, m1);
    return pow((c1 + c2 * Ym) / (1.0 + c3 * Ym), m2);
}

float pqToNits(float E)
{
    return pqToLinear(E) * 10000.0;
}

// IEC 61966-2-1 sRGB *EOTF* (encoded → linear), extended for |c| > 1.
// This is the inverse of the OETF (NOT pow(c, 1/2.4) / not OETF).
// Verified against OCIO Display P3 Un-tone-mapped: code 1.825 → linear 4.0.
// Primaries stay Display P3; only the transfer is decoded here.
float srgbPiecewiseToLinear(float c)
{
    float a = abs(c);
    float lin;
    // Threshold 0.04045 is on *encoded* values (EOTF). Do not use 0.0031308 here.
    if (a <= 0.04045)
        lin = a / 12.92;
    else
        lin = pow((a + 0.055) / 1.055, 2.4);
    return (c < 0.0) ? -lin : lin;
}

void main()
{
    vec4 c = texture(tex, v_uv);
    float s = params.x;
    int mode = int(params.y + 0.5);
    float sdrWhite = max(params.z, 1.0);

    if (mode == 1 && s > 1.0001)
    {
        // PQ HDR10: compensate Wayland SDR-white (203-nit) mismatch
        c.r = clamp(linearToPq(pqToLinear(c.r) * s), 0.0, 1.0);
        c.g = clamp(linearToPq(pqToLinear(c.g) * s), 0.0, 1.0);
        c.b = clamp(linearToPq(pqToLinear(c.b) * s), 0.0, 1.0);
    }
    else if (mode == 2)
    {
        // Linear extended surface from a PQ-encoded buffer:
        // 1.0 = compositor SDR/reference white.
        c.r = pqToNits(c.r) / sdrWhite;
        c.g = pqToNits(c.g) / sdrWhite;
        c.b = pqToNits(c.b) / sdrWhite;
    }
    else if (mode == 3)
    {
        // Display P3 Extended (macOS EDR-style):
        // Buffer is P3 + D65 + piecewise sRGB TF → linear P3.
        // Then ×s so content paper white (encoded 1.0 → lin 1.0) matches compositor
        // SDR UI white (Wayland often maps linear 1.0 dimmer than UI white without this).
        float sMatch = max(s, 1.0);
        c.r = srgbPiecewiseToLinear(c.r) * sMatch;
        c.g = srgbPiecewiseToLinear(c.g) * sMatch;
        c.b = srgbPiecewiseToLinear(c.b) * sMatch;
    }

    fragColor = c;
}
