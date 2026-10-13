// Proof vertex shader: one triangle from SV_VertexID, no vertex buffer.
// Covers the bottom-left half of the target (the build applies -fvk-invert-y).
float4 main(uint id : SV_VertexID) : SV_Position
{
    float2 p[3] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(-1.0, 1.0) };
    return float4(p[id], 0.0, 1.0);
}
