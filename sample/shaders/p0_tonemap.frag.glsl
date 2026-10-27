#version 450

/* V09 HDR resolve/tonemap: linear RGBA16F -> displayable RGBA8. */
layout(set = 3, binding = 0) uniform ToneParams
{
    /* x: exposure, y: gamma, z/w: reserved. */
    vec4 params;
} tone;

layout(set = 2, binding = 0) uniform sampler2D scene_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

vec3 aces_filmic(vec3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
    vec3 hdr = texture(scene_tex, v_uv).rgb * tone.params.x;
    vec3 mapped = aces_filmic(hdr);
    mapped = pow(max(mapped, vec3(0.0)), vec3(1.0 / max(tone.params.y, 0.001)));
    out_color = vec4(mapped, 1.0);
}
