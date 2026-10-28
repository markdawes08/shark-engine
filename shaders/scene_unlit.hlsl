// Engine-owned temporary unlit scene profile; no lighting or transparency model.
// Matrices already contain 13c's single projection-Y inversion.
struct SceneConstants {
    column_major float4x4 worldViewProjection;
    float4 baseColor;
};
[[vk::binding(256, 0)]] ConstantBuffer<SceneConstants> scene : register(b0);

struct SceneVertexInput {
    float3 position : POSITION;
    float4 color : COLOR0;
};

struct SceneVertexOutput {
    float4 position : SV_Position;
    float4 color : COLOR0;
};

SceneVertexOutput vsScene(SceneVertexInput input) {
    SceneVertexOutput output;
    output.position = mul(scene.worldViewProjection, float4(input.position, 1));
    output.color = input.color * scene.baseColor;
    return output;
}

float4 psScene(SceneVertexOutput input) : SV_Target {
    return input.color;
}
