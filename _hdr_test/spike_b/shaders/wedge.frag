#version 440

// Stepped EDR wedge. The surface is HDRExtendedDisplayP3Linear, so 1.0 is
// SDR white (paper white) and values above it are the extended range. On an
// SDR path every step >= 1.0 clips to the same white; on a working EDR path
// they climb visibly. That difference is the whole result of this spike.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf
{
    float headroom; // reported EDR headroom, for the marker bar
    float pad0;
    float pad1;
    float pad2;
}
ubuf;

const int STEP_COUNT = 8;
const float STEPS[STEP_COUNT] = float[](0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 8.0);

void main()
{
    // Bottom eighth: a marker showing where the display's reported headroom
    // falls on the same scale, so you can tell "clipped" from "out of range".
    if (vUV.y < 0.125)
    {
        float x = vUV.x * float(STEP_COUNT);
        int idx = int(clamp(x, 0.0, float(STEP_COUNT) - 1.0));
        float v = STEPS[idx] <= ubuf.headroom ? 1.0 : 0.0;
        fragColor = vec4(v, v * 0.35, 0.0, 1.0);
        return;
    }

    float x = vUV.x * float(STEP_COUNT);
    int idx = int(clamp(x, 0.0, float(STEP_COUNT) - 1.0));
    float v = STEPS[idx];
    fragColor = vec4(v, v, v, 1.0);
}
