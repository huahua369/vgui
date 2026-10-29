// V09 scene fragment shader.
//
// The tint uniform is pushed per draw: LDR views use (1,1,1,1), HDR views
// push values above 1 so the RGBA16F target really stores highlights that a
// later tonemap pass can roll off instead of clipping in the 8-bit target.

struct SceneParams
{
    float4 tint;
};

[[vk::binding(0, 3)]] ConstantBuffer<SceneParams> params;
[[vk::binding(0, 2)]] Sampler2D scene_tex;

struct FragmentInput
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] float4 color : COLOR0;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target0;
};

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    FragmentOutput output;
    output.color = input.color * scene_tex.Sample(input.uv) * params.tint;
    return output;
}
