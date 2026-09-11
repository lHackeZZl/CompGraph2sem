struct Particle
{
    float3 Position; float Age;
    float3 Velocity; float Lifetime;
    float4 Color;
    float Size; float3 Padding;
};

ConsumeStructuredBuffer<Particle> gInputParticles : register(u0);
AppendStructuredBuffer<Particle>  gOutputParticles : register(u1);

cbuffer UpdateConstants : register(b0)
{
    float gDeltaTime;
    float gTotalTime;
    uint  gParticleCount;
    float gPad0;
    float3 gEmitterPosition;
    float gPad1;
};

float Hash(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (value & 0x00ffffffu) / 16777215.0f;
}

void Respawn(inout Particle p, uint id)
{
    uint timeSeed = (uint)(gTotalTime * 1000.0f);
    float a = Hash(id * 3u + timeSeed) * 6.2831853f;
    float radial = 0.25f + Hash(id * 5u + timeSeed + 17u) * 1.15f;
    float speed = 2.8f + Hash(id * 7u + timeSeed + 31u) * 3.8f;
    p.Position = gEmitterPosition + float3(cos(a), 0.0f, sin(a)) * 0.18f;
    p.Velocity = float3(cos(a) * radial, speed, sin(a) * radial);
    p.Age = 0.0f;
    p.Lifetime = 1.8f + Hash(id * 11u + timeSeed + 47u) * 2.4f;
    float hue = Hash(id * 13u + 7u);
    p.Color = float4(
        1.0f,
        lerp(0.18f, 0.85f, hue),
        lerp(0.03f, 0.25f, hue),
        1.0f);
    p.Size = lerp(0.055f, 0.13f, Hash(id * 17u + timeSeed));
}

[numthreads(256, 1, 1)]
void CS(uint3 dispatchId : SV_DispatchThreadID)
{
    if (dispatchId.x >= gParticleCount)
        return;

    Particle particle = gInputParticles.Consume();
    particle.Age += gDeltaTime;
    particle.Velocity.y -= 3.8f * gDeltaTime;
    particle.Position += particle.Velocity * gDeltaTime;

    if (particle.Age >= particle.Lifetime || particle.Position.y < -1.45f)
        Respawn(particle, dispatchId.x);

    gOutputParticles.Append(particle);
}
