// gbuffer.hlsl — Geometry Pass: write G-Buffer
// RT0: World Position (xyz)  + unused
// RT1: World Normal   (xyz)  + unused
// RT2: Albedo (rgb)          + Shininess (a) packed as shininess/512

cbuffer CBPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4   gMatAmbient;
    float4   gMatDiffuse;
    float4   gMatSpecular;   // .w = shininess
    float    gAnimEnabled;
    float3   gObjPad;
};

cbuffer CBPerPass : register(b1)
{
    float4x4 gView;
    float4x4 gProj;
    float4x4 gViewProj;
    float3   gEyePosW;   float gPad0;
    float2   gTexScale;
    float2   gTexOffset;
    float    gTime;
    float3   gPad1;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VertexIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float2 TexCoord : TEXCOORD;
};

struct VertexOut
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float2 TexCoord : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float3 pos = vin.PosL;
    float3 nrm = vin.NormalL;

    // Vertex animation (картина + светильники)
    if (gAnimEnabled > 0.5f)
    {
        float wave = sin(pos.x * 4.0f + gTime * 2.5f)
                   * cos(pos.z * 3.0f + gTime * 1.8f)
                   * 0.04f;
        pos.y += wave;
        nrm.y += wave * 0.5f;
    }

    float4 posW = mul(float4(pos, 1.0f), gWorld);
    vout.PosW   = posW.xyz;
    vout.PosH   = mul(mul(posW, gView), gProj);
    vout.NormalW= mul(normalize(nrm), (float3x3)gWorldInvTranspose);
    vout.TexCoord = vin.TexCoord * gTexScale + gTexOffset;
    return vout;
}

struct GBufferOut
{
    float4 Position : SV_TARGET0;   // World pos (xyz) + unused
    float4 Normal   : SV_TARGET1;   // World normal (xyz) + unused
    float4 Albedo   : SV_TARGET2;   // Albedo (rgb) + shininess/512 (a)
};

GBufferOut PS(VertexOut pin)
{
    GBufferOut gout;

    float4 texColor = gTexture.Sample(gSampler, pin.TexCoord);
    float3 albedo   = texColor.rgb * gMatDiffuse.rgb;

    gout.Position = float4(pin.PosW, 1.0f);
    gout.Normal   = float4(normalize(pin.NormalW), 0.0f);
    gout.Albedo   = float4(albedo, gMatSpecular.w / 512.0f);  // shininess packed

    return gout;
}
