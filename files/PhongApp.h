#pragma once
#include "d3dApp.h"
#include "MathHelper.h"
#include "UploadBuffer.h"
#include "GeometryGenerator.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// ── Constant buffers ──────────────────────────────────────────────────────────
struct CBPerObject
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 WorldInvTranspose;
};

struct CBPerPass
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT3   EyePosW;   float Pad0=0;
    XMFLOAT3   LightDir;  float Pad1=0;
    XMFLOAT4   LightAmbient, LightDiffuse, LightSpecular;
    XMFLOAT4   MatAmbient, MatDiffuse, MatSpecular;
    XMFLOAT2   TexScale;   // тайлинг
    XMFLOAT2   TexOffset;  // UV-анимация
};

// ── Vertex (Position + Normal + UV) ──────────────────────────────────────────
struct Vertex
{
    XMFLOAT3 Pos;
    XMFLOAT3 Normal;
    XMFLOAT2 TexCoord;
};

// ── OBJ Material ──────────────────────────────────────────────────────────────
struct ObjMaterial
{
    std::string Name;
    XMFLOAT4 Ambient  = {0.2f,0.2f,0.2f,1};
    XMFLOAT4 Diffuse  = {0.8f,0.8f,0.8f,1};
    XMFLOAT4 Specular = {1,1,1,32};
    std::string DiffuseMap;
};

// ── Render item ───────────────────────────────────────────────────────────────
struct RenderItem
{
    XMFLOAT4X4 World = MathHelper::Identity4x4();
    int   NumFramesDirty = 3;
    UINT  ObjCBIndex     = 0;
    MeshGeometry* Geo    = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT IndexCount=0, StartIndexLocation=0;
    INT  BaseVertexLocation=0;
    // материал OBJ
    ObjMaterial Mat;
};

// ── Frame resource ────────────────────────────────────────────────────────────
struct FrameResource
{
    FrameResource(ID3D12Device* d, UINT pass, UINT obj);
    ComPtr<ID3D12CommandAllocator>             CmdListAlloc;
    std::unique_ptr<UploadBuffer<CBPerPass>>   PassCB;
    std::unique_ptr<UploadBuffer<CBPerObject>> ObjectCB;
    UINT64 Fence=0;
};

// ── Main app ──────────────────────────────────────────────────────────────────
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

    void BuildRootSignature();
    void BuildDescriptorHeaps();
    void BuildConstantBufferViews();
    void BuildShadersAndInputLayout();
    void BuildGeometry();
    void BuildPSO();
    void BuildFrameResources();
    void BuildRenderItems();

    // Текстура
    void CreateProceduralTexture();   // шахматная, если нет файла
    bool LoadTextureFromFile(const std::wstring& path);
    void CreateTextureViews();

    // OBJ+MTL
    bool LoadObjModel(const std::string& objFile);
    std::unordered_map<std::string, ObjMaterial> LoadMtlFile(const std::string& path);

    void UpdateObjectCBs(const GameTimer& gt);
    void UpdatePassCB   (const GameTimer& gt);
    void DrawRenderItems(ID3D12GraphicsCommandList* cmd);

    // Camera
    float mTheta=1.3f*XM_PI, mPhi=0.4f*XM_PI, mRadius=5.f;
    POINT mLastMousePos{};

    // Frame resources
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource=nullptr;
    int mCurrFrameResourceIndex=0;

    // Pipeline
    ComPtr<ID3D12DescriptorHeap> mCbvSrvHeap;  // CBV + SRV в одной куче
    ComPtr<ID3D12RootSignature>  mRootSignature;
    ComPtr<ID3D12PipelineState>  mPSO;
    ComPtr<ID3DBlob> mVsByteCode, mPsByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // Texture
    ComPtr<ID3D12Resource> mTexture, mTextureUpload;
    UINT mSrvOffset = 0;   // индекс SRV в куче

    // Geometry & render items
    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::vector<std::unique_ptr<RenderItem>> mAllRItems;
    std::vector<RenderItem*>                 mOpaqueRItems;

    // Pass state
    CBPerPass mMainPassCB{};
    UINT      mPassCbvOffset=0;
    float     mUVOffset=0.f;   // UV-анимация
};
