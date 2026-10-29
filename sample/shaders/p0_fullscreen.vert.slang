// V09 fullscreen triangle shared by the tonemap and depth-preview passes.
// No vertex buffer is required; the pipeline is drawn with 3 vertices.

struct VertexOutput
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    float4 position : SV_Position;
};

[shader("vertex")]
// SV_VulkanVertexID maps to the bare gl_VertexIndex (no gl_BaseVertex
// subtraction), which keeps the generated GLSL at 450 and SPIR-V at 1.0.
VertexOutput main(uint vertex_id : SV_VulkanVertexID)
{
    float2 p = float2(float((vertex_id << 1) & 2), float(vertex_id & 2));

    VertexOutput output;
    output.uv = p;
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return output;
}
