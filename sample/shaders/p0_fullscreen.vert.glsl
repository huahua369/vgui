#version 450

/* V09 fullscreen triangle shared by the tonemap and depth-preview passes.
 * No vertex buffer is required; the pipeline is drawn with 3 vertices. */
layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
