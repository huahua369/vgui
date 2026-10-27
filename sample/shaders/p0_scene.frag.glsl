#version 450

/* V09 scene fragment shader.
 *
 * The tint uniform is pushed per draw: LDR views use (1,1,1,1), HDR views
 * push values above 1 so the RGBA16F target really stores highlights that a
 * later tonemap pass can roll off instead of clipping in the 8-bit target. */
layout(set = 3, binding = 0) uniform SceneParams
{
    vec4 tint;
} params;

layout(set = 2, binding = 0) uniform sampler2D scene_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 out_color;

void main()
{
    out_color = v_color * texture(scene_tex, v_uv) * params.tint;
}
