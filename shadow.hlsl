// Depth-only shader used by all cascades. Object CB layout starts with gWorld,
// matching CBPerObject; the rest of that buffer is intentionally unused here.
cbuffer CBPerObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer CBShadowPass : register(b1)
{
    float4x4 gLightViewProj;
};

struct VertexIn
{
    float3 PosL : POSITION;
    float3 NormalL : NORMAL;
    float2 TexCoord : TEXCOORD;
};

float4 VS(VertexIn vin) : SV_POSITION
{
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    return mul(posW, gLightViewProj);
}
