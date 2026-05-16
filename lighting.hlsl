// lighting.hlsl — Lighting Pass: Phong + Hemisphere Ambient (fake soft shadows)
// Supports: 1 Directional + 3 Point + 1 Spot light

// ─── Light structures ─────────────────────────────────────────────────────────
struct DirectionalLight
{
    float3 Direction;  float  Pad0;
    float4 Ambient;
    float4 Diffuse;
    float4 Specular;
};

struct PointLight
{
    float3 Position;   float  Range;
    float4 Ambient;
    float4 Diffuse;
    float4 Specular;
    float3 Attenuation; float Pad1;
};

struct SpotLight
{
    float3 Position;   float  Range;
    float3 Direction;  float  SpotPower;
    float4 Ambient;
    float4 Diffuse;
    float4 Specular;
    float3 Attenuation; float Pad2;
};

// ─── Constant buffers ─────────────────────────────────────────────────────────
cbuffer CBLighting : register(b0)
{
    DirectionalLight gDirLight;
    PointLight       gPointLights[3];
    SpotLight        gSpotLight;
    float3           gEyePosW;   float gPad0;
    int              gNumPointLights;
    int              gHasSpot;
    float2           gLightPad;
};

// ─── G-Buffer textures ────────────────────────────────────────────────────────
Texture2D    gGBufPosition : register(t0);
Texture2D    gGBufNormal   : register(t1);
Texture2D    gGBufAlbedo   : register(t2);
SamplerState gSampler      : register(s0);

// ─── Vertex shader ────────────────────────────────────────────────────────────
struct QuadVIn  { float3 PosL : POSITION; float2 Tex : TEXCOORD; };
struct QuadVOut { float4 PosH : SV_POSITION; float2 Tex : TEXCOORD; };

QuadVOut VS(QuadVIn vin)
{
    QuadVOut vout;
    vout.PosH = float4(vin.PosL, 1.0f);
    vout.Tex  = vin.Tex;
    return vout;
}

// ─── Hemisphere Ambient ───────────────────────────────────────────────────────
// Имитирует soft shadowing: поверхности смотрящие вниз темнее (тень),
// поверхности смотрящие вверх — светлее (отражают свет с потолка).
float4 HemisphereAmbient(float3 N, float3 albedo)
{
    float3 skyColor    = float3(0.85f, 0.85f, 0.80f); // тёплый потолок/лампы
    float3 groundColor = float3(0.04f, 0.04f, 0.05f); // тёмный пол/тень

    // t=1 => нормаль смотрит вверх (много света)
    // t=0 => нормаль смотрит вниз (тень)
    float  t    = N.y * 0.5f + 0.5f;
    float3 hemi = lerp(groundColor, skyColor, t);
    return float4(hemi * albedo * 0.28f, 0.0f);
}

// ─── Phong helpers ────────────────────────────────────────────────────────────
float4 ComputeDirectional(DirectionalLight L, float3 N, float3 V,
                          float3 albedo, float shininess)
{
    float3 lightDir = normalize(-L.Direction);
    float  NdL      = max(dot(N, lightDir), 0.0f);

    float4 diffuse  = NdL * L.Diffuse * float4(albedo, 1.0f);

    float3 R   = reflect(-lightDir, N);
    float  spec = (NdL > 0.0f) ? pow(max(dot(R, V), 0.0f), shininess) : 0.0f;
    float4 specular = spec * L.Specular;

    return diffuse + specular;
}

float4 ComputePoint(PointLight L, float3 pos, float3 N, float3 V,
                    float3 albedo, float shininess)
{
    float3 toLight = L.Position - pos;
    float  dist    = length(toLight);
    if (dist > L.Range) return float4(0,0,0,0);
    toLight /= dist;

    float  NdL     = max(dot(N, toLight), 0.0f);
    float4 diffuse = NdL * L.Diffuse * float4(albedo, 1.0f);

    float3 R    = reflect(-toLight, N);
    float  spec = (NdL > 0.0f) ? pow(max(dot(R, V), 0.0f), shininess) : 0.0f;
    float4 specular = spec * L.Specular;

    float att = 1.0f / (L.Attenuation.x
                      + L.Attenuation.y * dist
                      + L.Attenuation.z * dist * dist);
    att = min(att, 1.0f);

    return (diffuse + specular) * att;
}

float4 ComputeSpot(SpotLight L, float3 pos, float3 N, float3 V,
                   float3 albedo, float shininess)
{
    float3 toLight = L.Position - pos;
    float  dist    = length(toLight);
    if (dist > L.Range) return float4(0,0,0,0);
    toLight /= dist;

    float  NdL     = max(dot(N, toLight), 0.0f);
    float4 diffuse = NdL * L.Diffuse * float4(albedo, 1.0f);

    float3 R    = reflect(-toLight, N);
    float  spec = (NdL > 0.0f) ? pow(max(dot(R, V), 0.0f), shininess) : 0.0f;
    float4 specular = spec * L.Specular;

    float cosAngle   = dot(-toLight, normalize(L.Direction));
    float spotFactor = pow(max(cosAngle, 0.0f), L.SpotPower);
    float att = spotFactor / (L.Attenuation.x
                             + L.Attenuation.y * dist
                             + L.Attenuation.z * dist * dist);
    att = min(att, 1.0f);

    return (diffuse + specular) * att;
}

// ─── Pixel shader ─────────────────────────────────────────────────────────────
float4 PS(QuadVOut pin) : SV_TARGET
{
    float3 posW    = gGBufPosition.Sample(gSampler, pin.Tex).rgb;
    float3 normalW = normalize(gGBufNormal.Sample(gSampler, pin.Tex).rgb);
    float4 albedoS = gGBufAlbedo.Sample(gSampler, pin.Tex);
    float3 albedo  = albedoS.rgb;
    float  shininess = max(albedoS.a * 512.0f, 1.0f);

    // Пустые пиксели — фоновый цвет
    if (dot(normalW, normalW) < 0.01f)
        return float4(0.10f, 0.10f, 0.13f, 1.0f);

    float3 V = normalize(gEyePosW - posW);

    // ── Hemisphere ambient: нижние грани темнее — имитация мягких теней ───────
    float4 color = HemisphereAmbient(normalW, albedo);

    // ── Directional ambient (базовая подсветка чтобы тени не были чёрными) ────
    color += gDirLight.Ambient * float4(albedo, 1.0f) * 0.35f;

    // ── Directional diffuse + specular ────────────────────────────────────────
    color += ComputeDirectional(gDirLight, normalW, V, albedo, shininess);

    // ── Point lights ──────────────────────────────────────────────────────────
    for (int i = 0; i < gNumPointLights; ++i)
        color += ComputePoint(gPointLights[i], posW, normalW, V, albedo, shininess);

    // ── Spot light ────────────────────────────────────────────────────────────
    if (gHasSpot)
        color += ComputeSpot(gSpotLight, posW, normalW, V, albedo, shininess);

    // ── Reinhard tone mapping ─────────────────────────────────────────────────
    color.rgb = color.rgb / (color.rgb + float3(1.0f, 1.0f, 1.0f));
    color.a   = 1.0f;
    return color;
}
