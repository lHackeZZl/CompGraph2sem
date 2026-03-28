// phong.hlsl — Phong + Texture + Tiling + UV Animation

cbuffer CBPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gWorldInvTranspose;
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
    float4   gMatAmbient;
    float4   gMatDiffuse;
    float4   gMatSpecular;   // .w = shininess
    float2   gTexScale;      // тайлинг
    float2   gTexOffset;     // UV-анимация (меняется каждый кадр)
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
    float4 posW  = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW    = posW.xyz;
    vout.PosH    = mul(mul(posW, gView), gProj);
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorldInvTranspose);
    // Тайлинг + UV-анимация
    vout.TexCoord = vin.TexCoord * gTexScale + gTexOffset;
    return vout;
}

float4 PS(VertexOut pin) : SV_TARGET
{
    float3 N = normalize(pin.NormalW);
    float3 L = normalize(gLightDir);
    float3 V = normalize(gEyePosW - pin.PosW);

    // Сэмплируем текстуру
    float4 texColor = gTexture.Sample(gSampler, pin.TexCoord);

    // 1. Ambient
    float4 ambient = gLightAmbient * gMatAmbient * texColor;

    // 2. Diffuse (Lambertian)
    float  NdotL   = max(dot(N, L), 0.0f);
    float4 diffuse = NdotL * gLightDiffuse * gMatDiffuse * texColor;

    // 3. Specular (Phong)
    float3 R       = reflect(-L, N);
    float  RdotV   = max(dot(R, V), 0.0f);
    float4 specular = (NdotL > 0.0f)
        ? pow(RdotV, gMatSpecular.w) * gLightSpecular * float4(gMatSpecular.rgb, 1.0f)
        : float4(0,0,0,0);

    // Заполняющий свет (fill light)
    float3 Lf   = normalize(float3(-0.5f, 0.3f, -0.7f));
    float  fill = max(dot(N, Lf), 0.0f) * 0.25f;
    float4 fillCol = float4(fill,fill,fill*1.3f, 0.0f) * texColor;

    float4 color = ambient + diffuse + specular + fillCol;
    color.a = gMatDiffuse.a;
    return color;
}
