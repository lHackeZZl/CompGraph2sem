#pragma once
#include <DirectXMath.h>
#include <vector>
#include <cstdint>
using namespace DirectX;

class GeometryGenerator
{
public:
    struct Vertex
    {
        Vertex() = default;
        Vertex(float px, float py, float pz,
               float nx, float ny, float nz,
               float u,  float v)
            : Position(px,py,pz), Normal(nx,ny,nz), TexCoord(u,v) {}

        XMFLOAT3 Position;
        XMFLOAT3 Normal;
        XMFLOAT2 TexCoord;
    };

    struct MeshData
    {
        std::vector<Vertex>   Vertices;
        std::vector<uint32_t> Indices32;

        std::vector<uint16_t>& GetIndices16()
        {
            if (mIndices16.empty()) {
                mIndices16.resize(Indices32.size());
                for (size_t i = 0; i < Indices32.size(); ++i)
                    mIndices16[i] = (uint16_t)Indices32[i];
            }
            return mIndices16;
        }
    private:
        std::vector<uint16_t> mIndices16;
    };

    MeshData CreateBox(float w, float h, float d, uint32_t subdivisions);
};
