// Shark shader convention 1. Engine-owned source under the repository license.
// Matrices use column-major storage and matrix * column-vector multiplication.
struct Globals {
    column_major float4x4 worldViewProjection;
};
[[vk::binding(256, 0)]] ConstantBuffer<Globals> globals : register(b0);
