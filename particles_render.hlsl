struct Particle
{
    float3 Position; float Age;
    float3 Velocity; float Lifetime;
    float4 Color;
    float Size; float3 Padding;
};

StructuredBuffer<Particle> gParticles : register(t0);

cbuffer RenderConstants : register(b0)
{
    float4x4 gViewProjection;
    float3 gCameraRight; float gParticleScale;
    float3 gCameraUp; float gPad0;
    float3 gCameraForward; float gPad1;
};

struct VSOut
{
    float3 CenterW : POSITION;
    float4 Color : COLOR;
    float Size : SIZE;
};

VSOut VS(uint vertexId : SV_VertexID)
{
    Particle particle = gParticles[vertexId];
    VSOut output;
    output.CenterW = particle.Position;
    output.Color = particle.Color;
    output.Size = particle.Size * gParticleScale;
    return output;
}

struct GSOut
{
    float4 PositionH : SV_POSITION;
    float3 PositionW : POSITION;
    float3 NormalW : NORMAL;
    float4 Color : COLOR;
    float2 UV : TEXCOORD;
};

[maxvertexcount(4)]
void GS(point VSOut input[1], inout TriangleStream<GSOut> stream)
{
    const float2 corners[4] = {
        float2(-1.0f, -1.0f), float2(-1.0f, 1.0f),
        float2( 1.0f, -1.0f), float2( 1.0f, 1.0f)
    };
    const float2 uvs[4] = {
        float2(0.0f, 1.0f), float2(0.0f, 0.0f),
        float2(1.0f, 1.0f), float2(1.0f, 0.0f)
    };

    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        GSOut output;
        float3 positionW = input[0].CenterW
            + gCameraRight * (corners[i].x * input[0].Size)
            + gCameraUp * (corners[i].y * input[0].Size);
        output.PositionW = positionW;
        output.PositionH = mul(float4(positionW, 1.0f), gViewProjection);
        output.NormalW = normalize(-gCameraForward);
        output.Color = input[0].Color;
        output.UV = uvs[i];
        stream.Append(output);
    }
}

struct GBufferOut
{
    float4 Position : SV_TARGET0;
    float4 Normal : SV_TARGET1;
    float4 Albedo : SV_TARGET2;
};

GBufferOut PS(GSOut input)
{
    float2 circle = input.UV * 2.0f - 1.0f;
    clip(1.0f - dot(circle, circle));

    GBufferOut output;
    output.Position = float4(input.PositionW, 1.0f);
    output.Normal = float4(normalize(input.NormalW), 0.0f);
    output.Albedo = float4(input.Color.rgb, 32.0f / 512.0f);
    return output;
}
