// V09 depth texture preview: scene depth (D32_FLOAT) -> displayable RGBA8.
// Near samples are blue, far samples warm orange, the cleared background is
// dark.  The linearisation uses the sample's fixed 0.1/100 clip planes.

struct DepthParams
{
    // x: near plane, y: far plane, z/w: reserved.
    float4 params;
};

[[vk::binding(0, 3)]] ConstantBuffer<DepthParams> depth;
[[vk::binding(0, 2)]] Sampler2D depth_tex;

struct FragmentInput
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target0;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    float d = depth_tex.Sample(input.uv).r;
    if (d >= 0.99999)
    {
        FragmentOutput output;
        output.color = float4(0.02, 0.025, 0.035, 1.0);
        return output;
    }

    float near_plane = max(depth.params.x, 0.0001);
    float far_plane = max(depth.params.y, near_plane + 0.0001);
    float linear = (near_plane * far_plane) /
        max(far_plane - d * (far_plane - near_plane), 0.0001);

    // Compress the useful 0..20 unit range into a readable gradient.
    float t = clamp(1.0 - exp(-linear * 0.22), 0.0, 1.0);
    float3 near_color = float3(0.10, 0.40, 0.95);
    float3 far_color = float3(0.98, 0.55, 0.12);
    float3 color = lerp(near_color, far_color, t);

    // Thin contour bands make the depth structure easier to read.
    float band = frac(linear * 0.5);
    color *= 0.85 + 0.15 * smoothstep(0.0, 0.15, band);

    FragmentOutput output;
    output.color = float4(color, 1.0);
    return output;
}
