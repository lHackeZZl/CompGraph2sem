// gbuffer.hlsl — Geometry Pass: G-Buffer + normal map + displacement tessellation
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
    float    gUseNormalMap;
    float    gUseDisplacementMap;
    float    gDisplacementScale;
    float    gTessNear;
    float    gTessFar;
    float    gTessMin;
    float    gTessMax;
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

Texture2D    gTexture         : register(t0);
Texture2D    gNormalMap       : register(t1);
Texture2D    gDisplacementMap : register(t2);
SamplerState gSampler         : register(s0);

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

float3 AnimatePosition(float3 pos, inout float3 nrm)
{
    // Vertex animation (картина + светильники)
    if (gAnimEnabled > 0.5f)
    {
        float wave = sin(pos.x * 4.0f + gTime * 2.5f)
                   * cos(pos.z * 3.0f + gTime * 1.8f)
                   * 0.04f;
        pos.y += wave;
        nrm.y += wave * 0.5f;
    }
    return pos;
}

// ─── Regular geometry path ───────────────────────────────────────────────────
VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    float3 pos = vin.PosL;
    float3 nrm = vin.NormalL;
    pos = AnimatePosition(pos, nrm);

    float4 posW = mul(float4(pos, 1.0f), gWorld);
    vout.PosW    = posW.xyz;
    vout.PosH    = mul(mul(posW, gView), gProj);
    vout.NormalW = mul(normalize(nrm), (float3x3)gWorldInvTranspose);
    vout.TexCoord = vin.TexCoord * gTexScale + gTexOffset;
    return vout;
}

// ─── Tessellation path ───────────────────────────────────────────────────────
// OBJ-сцена хранится как triangle list, поэтому тесселяция сделана через
// 3-control-point triangular patches. Для обычных объектов используется VS(),
// а для стен/картины PhongApp ставит topology = 3_CONTROL_POINT_PATCHLIST.
struct ControlPoint
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float2 TexCoord : TEXCOORD;
};

ControlPoint VSTess(VertexIn vin)
{
    ControlPoint vout;
    float3 pos = vin.PosL;
    float3 nrm = vin.NormalL;
    pos = AnimatePosition(pos, nrm);
    vout.PosL = pos;
    vout.NormalL = nrm;
    vout.TexCoord = vin.TexCoord;
    return vout;
}

struct PatchTess
{
    float EdgeTess[3]   : SV_TessFactor;
    float InsideTess[1] : SV_InsideTessFactor;
};

float3 ClosestPointOnTriangle(float3 p, float3 a, float3 b, float3 c)
{
    // Ericson, Real-Time Collision Detection: closest point to triangle.
    // Важно для больших стен: камера может быть вплотную к плоскости стены,
    // хотя центр треугольника далеко. Поэтому считаем дистанцию не до центра,
    // а до ближайшей точки патча.
    float3 ab = b - a;
    float3 ac = c - a;
    float3 ap = p - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    float3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f)
    {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }

    float3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f)
    {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }

    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)
    {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }

    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

float ComputeDistanceTess(float3 p0, float3 p1, float3 p2)
{
    float3 c0 = mul(float4(p0, 1.0f), gWorld).xyz;
    float3 c1 = mul(float4(p1, 1.0f), gWorld).xyz;
    float3 c2 = mul(float4(p2, 1.0f), gWorld).xyz;

    float3 closest = ClosestPointOnTriangle(gEyePosW, c0, c1, c2);
    float dist = distance(closest, gEyePosW);

    // Близко к поверхности — высокая тесселяция, далеко — низкая.
    // Нижний предел не опускаем до 1, иначе рельеф на стенах визуально пропадает.
    float t = saturate((dist - gTessNear) / max(gTessFar - gTessNear, 0.01f));
    return clamp(lerp(gTessMax, gTessMin, t), 2.0f, 64.0f);
}

PatchTess PatchConstantHS(InputPatch<ControlPoint, 3> patch,
                          uint patchID : SV_PrimitiveID)
{
    PatchTess pt;
    float tess = ComputeDistanceTess(patch[0].PosL, patch[1].PosL, patch[2].PosL);

    pt.EdgeTess[0] = tess;
    pt.EdgeTess[1] = tess;
    pt.EdgeTess[2] = tess;
    pt.InsideTess[0] = tess;
    return pt;
}

[domain("tri")]
[partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstantHS")]
ControlPoint HS(InputPatch<ControlPoint, 3> patch,
                uint i : SV_OutputControlPointID,
                uint patchID : SV_PrimitiveID)
{
    return patch[i];
}

[domain("tri")]
VertexOut DS(PatchTess pt,
             float3 bary : SV_DomainLocation,
             const OutputPatch<ControlPoint, 3> patch)
{
    VertexOut vout;

    float3 pos = patch[0].PosL    * bary.x + patch[1].PosL    * bary.y + patch[2].PosL    * bary.z;
    float3 nrm = patch[0].NormalL * bary.x + patch[1].NormalL * bary.y + patch[2].NormalL * bary.z;
    float2 tex = patch[0].TexCoord * bary.x + patch[1].TexCoord * bary.y + patch[2].TexCoord * bary.z;

    nrm = normalize(nrm);
    tex = tex * gTexScale + gTexOffset;

    if (gUseDisplacementMap > 0.5f)
    {
        float h = gDisplacementMap.SampleLevel(gSampler, tex, 0).r;

        // Увеличиваем контраст height map, чтобы это был именно геометрический
        // displacement, а не эффект, который визуально теряется в normal map.
        h = saturate((h - 0.5f) * 1.75f + 0.5f);
        pos += nrm * ((h - 0.5f) * gDisplacementScale);
    }

    float4 posW = mul(float4(pos, 1.0f), gWorld);
    vout.PosW    = posW.xyz;
    vout.PosH    = mul(mul(posW, gView), gProj);
    vout.NormalW = mul(normalize(nrm), (float3x3)gWorldInvTranspose);
    vout.TexCoord = tex;
    return vout;
}

// ─── Pixel shader and normal mapping ─────────────────────────────────────────
struct GBufferOut
{
    float4 Position : SV_TARGET0;
    float4 Normal   : SV_TARGET1;
    float4 Albedo   : SV_TARGET2;
};

float3 ApplyNormalMap(float3 normalW, float3 posW, float2 tex)
{
    float3 N = normalize(normalW);
    if (gUseNormalMap < 0.5f)
        return N;

    float3 normalTS = gNormalMap.Sample(gSampler, tex).xyz * 2.0f - 1.0f;

    float3 dp1  = ddx(posW);
    float3 dp2  = ddy(posW);
    float2 duv1 = ddx(tex);
    float2 duv2 = ddy(tex);

    float det = duv1.x * duv2.y - duv1.y * duv2.x;
    if (abs(det) < 1e-5f)
        return N;

    float3 T = normalize(dp1 * duv2.y - dp2 * duv1.y);
    T = normalize(T - N * dot(N, T));
    float3 B = normalize(cross(N, T));

    return normalize(normalTS.x * T + normalTS.y * B + normalTS.z * N);
}

GBufferOut PS(VertexOut pin)
{
    GBufferOut gout;

    float4 texColor = gTexture.Sample(gSampler, pin.TexCoord);
    float3 albedo   = texColor.rgb * gMatDiffuse.rgb;
    float3 normalW  = ApplyNormalMap(pin.NormalW, pin.PosW, pin.TexCoord);

    gout.Position = float4(pin.PosW, 1.0f);
    gout.Normal   = float4(normalW, 0.0f);
    gout.Albedo   = float4(albedo, gMatSpecular.w / 512.0f);

    return gout;
}
