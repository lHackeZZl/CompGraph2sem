#include "GeometryGenerator.h"
#include <algorithm>

GeometryGenerator::MeshData
GeometryGenerator::CreateBox(float w, float h, float d, uint32_t /*sub*/)
{
    MeshData mesh;
    float hw=w*.5f, hh=h*.5f, hd=d*.5f;

    // 24 vertices (4 per face) with UVs
    Vertex v[24] = {
        // Front  (z-)
        {-hw,-hh,-hd, 0,0,-1, 0,1}, { hw,-hh,-hd, 0,0,-1, 1,1},
        { hw, hh,-hd, 0,0,-1, 1,0}, {-hw, hh,-hd, 0,0,-1, 0,0},
        // Back   (z+)
        { hw,-hh, hd, 0,0,1, 0,1}, {-hw,-hh, hd, 0,0,1, 1,1},
        {-hw, hh, hd, 0,0,1, 1,0}, { hw, hh, hd, 0,0,1, 0,0},
        // Top    (y+)
        {-hw, hh,-hd, 0,1,0, 0,1}, { hw, hh,-hd, 0,1,0, 1,1},
        { hw, hh, hd, 0,1,0, 1,0}, {-hw, hh, hd, 0,1,0, 0,0},
        // Bottom (y-)
        {-hw,-hh, hd, 0,-1,0, 0,1}, { hw,-hh, hd, 0,-1,0, 1,1},
        { hw,-hh,-hd, 0,-1,0, 1,0}, {-hw,-hh,-hd, 0,-1,0, 0,0},
        // Left   (x-)
        {-hw,-hh, hd,-1,0,0, 0,1}, {-hw,-hh,-hd,-1,0,0, 1,1},
        {-hw, hh,-hd,-1,0,0, 1,0}, {-hw, hh, hd,-1,0,0, 0,0},
        // Right  (x+)
        { hw,-hh,-hd, 1,0,0, 0,1}, { hw,-hh, hd, 1,0,0, 1,1},
        { hw, hh, hd, 1,0,0, 1,0}, { hw, hh,-hd, 1,0,0, 0,0},
    };
    mesh.Vertices.assign(v, v+24);

    uint32_t idx[36] = {
        0,2,1, 0,3,2,
        4,6,5, 4,7,6,
        8,10,9, 8,11,10,
        12,14,13, 12,15,14,
        16,18,17, 16,19,18,
        20,22,21, 20,23,22,
    };
    mesh.Indices32.assign(idx, idx+36);
    return mesh;
}
