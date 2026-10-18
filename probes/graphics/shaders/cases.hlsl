// Shark assignment-02 analytic fixtures, licensed under the repository license.
// Explicit Vulkan bindings match the pinned NVRHI default binding offsets.
struct Globals {
    float4 transform; // XY scale, depth, fragment mode (0 textured, 1 constant).
    float4 color;
    uint4 control; // sampling mode, cube face/depth layer, cube mip, reserved.
};
[[vk::binding(256, 0)]] ConstantBuffer<Globals> globals : register(b0);
[[vk::binding(0, 0)]] Texture2D<float4> inputTexture : register(t0);
[[vk::binding(1, 0)]] Texture2D<float4> srgbTexture : register(t1);
[[vk::binding(2, 0)]] TextureCube<float4> cubeTexture : register(t2);
[[vk::binding(3, 0)]] Texture2DArray<float> depthTexture : register(t3);
[[vk::binding(128, 0)]] SamplerState linearSampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexOutput vsMain(uint vertex : SV_VertexID) {
    const float2 positions[4] = {
        float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1)
    };
    float2 position = positions[vertex];
    VertexOutput output;
    // Exactly one projection Y inversion. The NVRHI adapter supplies a positive
    // native Vulkan viewport; world winding remains counterclockwise.
    output.position = float4(position.x * globals.transform.x,
                             -position.y * globals.transform.y,
                             globals.transform.z, 1);
    output.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
    return output;
}

float4 psMain(VertexOutput input) : SV_Target {
    return globals.transform.w > 0.5 ? globals.color :
        inputTexture.SampleLevel(linearSampler, input.uv, 0) * globals.color;
}

float4 sampleMain(VertexOutput input) : SV_Target {
    if (globals.control.x == 0)
        return inputTexture.SampleLevel(linearSampler, input.uv, 0);
    if (globals.control.x == 1)
        return srgbTexture.SampleLevel(linearSampler, float2(0.5, 0.5), 0);
    if (globals.control.x == 2) {
        const float3 directions[6] = {
            float3(1,0,0), float3(-1,0,0), float3(0,1,0),
            float3(0,-1,0), float3(0,0,1), float3(0,0,-1)
        };
        return cubeTexture.SampleLevel(linearSampler, directions[globals.control.y],
                                       float(globals.control.z));
    }
    if (globals.control.x == 3) {
        // Depth filtering is deliberately not required by the platform profile.
        float depth = depthTexture.Load(int4(1024, 1024, globals.control.y, 0));
        return float4(depth, depth, depth, 1);
    }
    return inputTexture.SampleLevel(linearSampler, float2(0.5, 0.5), 0);
}

[[vk::binding(0, 0)]] StructuredBuffer<uint> inputValues : register(t0);
[[vk::binding(384, 0)]] RWStructuredBuffer<uint> outputValues : register(u0);
[[vk::binding(385, 0), vk::image_format("rgba16f")]] RWTexture2D<float4> outputImage : register(u1);

[numthreads(16, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID) {
    uint index = id.y * 16 + id.x;
    outputValues[index] = inputValues[index] * 3 + 11;
    outputImage[id.xy] = float4(2 + float(id.x) / 8, 4 + float(id.y) / 8, 8, 0.5);
}
