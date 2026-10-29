struct GuiGlobals
{
    float4 transform;
};
[[vk::binding(256, 0)]] ConstantBuffer<GuiGlobals> globals : register(b0);
[[vk::binding(0, 0)]] Texture2D<float4> atlas : register(t0);
[[vk::binding(128, 0)]] SamplerState atlasSampler : register(s0);

struct GuiInput
{
    float2 position : POSITION;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
struct GuiOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};
GuiOutput vsGui(GuiInput input)
{
    GuiOutput output;
    output.position = float4(input.position * globals.transform.xy + globals.transform.zw, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
float4 psGui(GuiOutput input) : SV_Target0
{
    return input.color * atlas.Sample(atlasSampler, input.uv);
}
