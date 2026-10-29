// V09 HDR resolve/tonemap: linear RGBA16F -> displayable RGBA8.

struct ToneParams
{
    // x: exposure, y: gamma, z/w: reserved.
    float4 params;
};

[[vk::binding(0, 3)]] ConstantBuffer<ToneParams> tone;
[[vk::binding(0, 2)]] Sampler2D scene_tex;

struct FragmentInput
{
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

struct FragmentOutput
{
    [[vk::location(0)]] float4 color : SV_Target0;
};

float3 aces_filmic(float3 x)
{
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

[shader("fragment")]
FragmentOutput main(FragmentInput input)
{
    float3 hdr = scene_tex.Sample(input.uv).rgb * tone.params.x;
    float3 mapped = aces_filmic(hdr);
    mapped = pow(max(mapped, float3(0.0)),
        float3(1.0 / max(tone.params.y, 0.001)));

    FragmentOutput output;
    output.color = float4(mapped, 1.0);
    return output;
}
