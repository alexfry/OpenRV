#version 440

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texcoord;

layout(location = 0) out vec2 v_uv;

// std140: mat4 @0 (64), vec4 params @64
// params.w = 1 → flip V (GL bottom-up readback without CPU flip)
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    vec4 params;
};

void main()
{
    v_uv = texcoord;
    if (params.w > 0.5)
        v_uv.y = 1.0 - v_uv.y;
    gl_Position = qt_Matrix * vec4(position, 0.0, 1.0);
}
