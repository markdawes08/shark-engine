// Offline shader building blocks for assignment 06; no GPU execution in 06a.
// Engine-owned source under the repository license.
#include "include/globals.hlsli"

[[vk::binding(0, 0)]] Texture2D<float4> colorTexture : register(t0);
[[vk::binding(128, 0)]] SamplerState colorSampler : register(s0);

struct VertexInput {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vsMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(globals.worldViewProjection, float4(input.position, 1));
    // A single projection Y inversion matches Shark's positive Vulkan viewport.
    output.position.y = -output.position.y;
    output.uv = input.uv;
    return output;
}

float4 psMain(VertexOutput input) : SV_Target {
    return colorTexture.Sample(colorSampler, input.uv);
}
