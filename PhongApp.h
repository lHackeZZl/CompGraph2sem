#pragma once
#include "d3dApp.h"
#include "MathHelper.h"
#include "UploadBuffer.h"
#include "GeometryGenerator.h"
#include "RenderingSystem.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ─── Geometry constant buffers ────────────────────────────────────────────────
struct CBPerObject
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 WorldInvTranspose;
    XMFLOAT4   MatAmbient;
    XMFLOAT4   MatDiffuse;
    XMFLOAT4   MatSpecular;   // .w = shininess
    float      AnimEnabled;
    XMFLOAT3   ObjPad;
};

struct CBPerPass
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT4X4 ViewProj;
    XMFLOAT3   EyePosW;  float Pad0 = 0;
    XMFLOAT2   TexScale;
    XMFLOAT2   TexOffset;
    float      Time = 0.f;
    float      Pad1x = 0, Pad1y = 0, Pad1z = 0;
};

struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
};

struct ObjMaterial
{
    std::string Name;
    XMFLOAT4 Ambient  = {0.2f,0.2f,0.2f,1};
    XMFLOAT4 Diffuse  = {0.8f,0.8f,0.8f,1};
    XMFLOAT4 Specular = {1,1,1,32};
    std::string DiffuseMap;
};

struct RenderItem
{
    XMFLOAT4X4 World = MathHelper::Identity4x4();
    int   NumFramesDirty = 3;
    UINT  ObjCBIndex     = 0;
    MeshGeometry* Geo    = nullptr;
    std::string   SubMesh;
    std::string   TextureName;
    ObjMaterial   Mat;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool AnimEnabled = false;
};

struct FrameResource
{
    FrameResource(ID3D12Device* d, UINT pass, UINT obj);
    ComPtr<ID3D12CommandAllocator>             CmdListAlloc;
    std::unique_ptr<UploadBuffer<CBPerPass>>   PassCB;
    std::unique_ptr<UploadBuffer<CBPerObject>> ObjectCB;
    UINT64 Fence = 0;
};

class PhongApp : public D3DApp
{
public:
    explicit PhongApp(HINSTANCE h);
    ~PhongApp() override;
    bool Initialize() override;

private:
    void OnResize() override;
    void Update(const GameTimer& gt) override;
    void Draw  (const GameTimer& gt) override;
    void OnMouseDown(WPARAM b,int x,int y) override;
    void OnMouseUp  (WPARAM b,int x,int y) override;
    void OnMouseMove(WPARAM b,int x,int y) override;

    void SetupLights();
    void BuildDescriptorHeaps();
    void BuildConstantBufferViews();
    void BuildTextureViews();
    void BuildFrameResources();

    bool LoadTexture(const std::string& name, const std::wstring& path);
    void CreateProceduralTexture(const std::string& name);
    bool LoadScene(const std::string& objFile);
    std::unordered_map<std::string,ObjMaterial> LoadMtl(const std::string& path);
    void BuildDefaultCube();
    void BuildLampMeshes(); // меши абажуров над Point Light 0 и 1

    void UpdateObjectCBs(const GameTimer& gt);
    void UpdatePassCB   (const GameTimer& gt);
    void DrawRenderItems(ID3D12GraphicsCommandList* cmd);

    // ── Camera ────────────────────────────────────────────────────────────────
    float mTheta  = 1.3f*XM_PI;
    float mPhi    = 0.4f*XM_PI;
    float mRadius = 8.f;
    POINT mLastMousePos{};

    // ── RenderingSystem (deferred) ────────────────────────────────────────────
    RenderingSystem mRenderer;
    CBLighting      mLightingData{};

    // ── Frame resources ───────────────────────────────────────────────────────
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource      = nullptr;
    int            mCurrFrameResourceIndex = 0;

    // ── Descriptor heaps ──────────────────────────────────────────────────────
    // mCbvSrvHeap layout:
    //   [0 .. n*3-1]         — object CBVs  (n objects × 3 frames)
    //   [n*3 .. n*3+2]       — pass   CBVs  (3 frames)
    //   [n*3+3 .. n*3+5]     — lighting CBVs(3 frames)
    //   [n*3+6 .. n*3+6+T-1] — texture SRVs (T textures)
    //   [last 3]             — G-Buffer SRVs (Position, Normal, Albedo)
    ComPtr<ID3D12DescriptorHeap> mCbvSrvHeap;
    ComPtr<ID3D12DescriptorHeap> mGBufferRtvHeap; // RTV heap for G-Buffer

    UINT mPassCbvOffset  = 0;
    UINT mLightCbvOffset = 0;
    UINT mSrvBaseOffset  = 0;
    UINT mGBufSrvOffset  = 0;

    // ── Textures ──────────────────────────────────────────────────────────────
    std::unordered_map<std::string, ComPtr<ID3D12Resource>> mTextures;
    std::unordered_map<std::string, ComPtr<ID3D12Resource>> mTextureUploads;
    std::unordered_map<std::string, UINT>                   mTextureSrvIndex;
    std::string mFallbackTexName = "__checkerboard__";

    // ── Geometry ──────────────────────────────────────────────────────────────
    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::vector<std::unique_ptr<RenderItem>> mAllRItems;
    std::vector<RenderItem*>                 mOpaqueRItems;

    float mTime = 0.f;
};
