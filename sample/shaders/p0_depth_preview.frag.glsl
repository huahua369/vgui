#version 450

/* V09 depth texture preview: scene depth (D32_FLOAT) -> displayable RGBA8.
 * Near samples are blue, far samples warm orange, the cleared background is
 * dark.  The linearisation uses the sample's fixed 0.1/100 clip planes. */
layout(set = 3, binding = 0) uniform DepthParams
{
    /* x: near plane, y: far plane, z/w: reserved. */
    vec4 params;
} depth;

layout(set = 2, binding = 0) uniform sampler2D depth_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main()
{
    float d = texture(depth_tex, v_uv).r;
    if (d >= 0.99999)
    {
        out_color = vec4(0.02, 0.025, 0.035, 1.0);
        return;
    }

    float near_plane = max(depth.params.x, 0.0001);
    float far_plane = max(depth.params.y, near_plane + 0.0001);
    float linear = (near_plane * far_plane) /
        max(far_plane - d * (far_plane - near_plane), 0.0001);

    /* Compress the useful 0..20 unit range into a readable gradient. */
    float t = clamp(1.0 - exp(-linear * 0.22), 0.0, 1.0);
    vec3 near_color = vec3(0.10, 0.40, 0.95);
    vec3 far_color = vec3(0.98, 0.55, 0.12);
    vec3 color = mix(near_color, far_color, t);

    /* Thin contour bands make the depth structure easier to read. */
    float band = fract(linear * 0.5);
    color *= 0.85 + 0.15 * smoothstep(0.0, 0.15, band);
    out_color = vec4(color, 1.0);
}
