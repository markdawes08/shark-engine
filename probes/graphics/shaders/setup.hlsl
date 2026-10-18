// Offline 02a compiler setup fixture. GPU execution and independent validation
// belong to 02b; a successful compilation alone is not a graphics gate.
RWStructuredBuffer<uint> values : register(u0, space0);

[numthreads(8, 1, 1)]
void main(uint3 dispatch_id : SV_DispatchThreadID)
{
    values[dispatch_id.x] = dispatch_id.x * 3u + 7u;
}
