cbuffer CBPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
    float4   gMatAmbient;
    float4   gMatDiffuse;
    float4   gMatSpecular;   // .w = shininess
    float    gAnimEnabled;   // 1.0 = анимация вкл для этого объекта
    float3   gObjPad;
};

cbuffer CBPerPass : register(b1)
{
    float4x4 gView;
    float4x4 gProj;
    float3   gEyePosW;   float gPad0;
    float3   gLightDir;  float gPad1;
    float4   gLightAmbient;
    float4   gLightDiffuse;
    float4   gLightSpecular;
    float4   gMatAmbientPass, gMatDiffusePass, gMatSpecularPass;
    float2   gTexScale;
    float2   gTexOffset;
    float    gTime;
    float3   gPad2;
};

Texture2D    gTexture : register(t0);
SamplerState gSampler : register(s0);

struct VertexIn  { float3 PosL:POSITION; float3 NormalL:NORMAL; float2 TexCoord:TEXCOORD; };
struct VertexOut { float4 PosH:SV_POSITION; float3 PosW:POSITION; float3 NormalW:NORMAL; float2 TexCoord:TEXCOORD; };

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float3 pos = vin.PosL;
    float3 nrm = vin.NormalL;

    // Вертексная анимация только если gAnimEnabled = 1
    // (картина качается, светильники покачиваются)
    if (gAnimEnabled > 0.5f)
    {
        float wave = sin(pos.x * 4.0f + gTime * 2.5f)
                   * cos(pos.z * 3.0f + gTime * 1.8f)
                   * 0.04f;
        pos.y += wave;
        nrm.y += wave * 0.5f;
    }

    float4 posW  = mul(float4(pos, 1.0f), gWorld);
    vout.PosW    = posW.xyz;
    vout.PosH    = mul(mul(posW, gView), gProj);
    vout.NormalW = mul(normalize(nrm), (float3x3)gWorldInvTranspose);
    vout.TexCoord = vin.TexCoord * gTexScale + gTexOffset;
    return vout;
}

float4 PS(VertexOut pin) : SV_TARGET
{
    float3 N = normalize(pin.NormalW);
    float3 L = normalize(gLightDir);
    float3 V = normalize(gEyePosW - pin.PosW);

    float4 texColor = gTexture.Sample(gSampler, pin.TexCoord);

    // Phong: Ambient + Diffuse + Specular
    float4 ambient  = gLightAmbient * gMatAmbient * texColor;
    float  NdotL    = max(dot(N, L), 0.0f);
    float4 diffuse  = NdotL * gLightDiffuse * gMatDiffuse * texColor;
    float3 R        = reflect(-L, N);
    float  RdotV    = max(dot(R, V), 0.0f);
    float4 specular = (NdotL > 0.0f)
        ? pow(RdotV, gMatSpecular.w) * gLightSpecular * float4(gMatSpecular.rgb, 1.0f)
        : float4(0,0,0,0);

    float fill  = max(dot(N, normalize(float3(-0.5f,0.3f,-0.7f))), 0.0f) * 0.2f;
    float4 color = ambient + diffuse + specular + float4(fill,fill,fill,0)*texColor;
    color.a = gMatDiffuse.a;
    return color;
}
