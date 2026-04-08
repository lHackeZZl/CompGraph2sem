#pragma once
#include "d3dApp.h"
#include "MathHelper.h"
#include "UploadBuffer.h"
#include "GeometryGenerator.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

struct CBPerObject
{
    XMFLOAT4X4 World;
    XMFLOAT4X4 WorldInvTranspose;
    XMFLOAT4   MatAmbient;
    XMFLOAT4   MatDiffuse;
    XMFLOAT4   MatSpecular; // .w = shininess
    float      AnimEnabled; // 1.0 = вертексная анимация вкл
    XMFLOAT3   ObjPad;
};

struct CBPerPass
{
    XMFLOAT4X4 View;
    XMFLOAT4X4 Proj;
    XMFLOAT3   EyePosW;   float Pad0=0;
    XMFLOAT3   LightDir;  float Pad1=0;
    XMFLOAT4   LightAmbient, LightDiffuse, LightSpecular;
    XMFLOAT4   MatAmbientPass, MatDiffusePass, MatSpecularPass;
    XMFLOAT2   TexScale;
    XMFLOAT2   TexOffset;
    float      Time=0.f;
    float      Pad2x=0, Pad2y=0, Pad2z=0;
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
    std::string DiffuseMap; // имя файла текстуры
};

struct RenderItem
{
    XMFLOAT4X4 World = MathHelper::Identity4x4();
    int   NumFramesDirty = 3;
    UINT  ObjCBIndex     = 0;
    MeshGeometry* Geo    = nullptr;
    std::string   SubMesh;          // ключ в DrawArgs
    std::string   TextureName;      // какую текстуру использовать
    ObjMaterial   Mat;
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool AnimEnabled = false; // вертексная анимация для этого объекта
};

struct FrameResource
{
    FrameResource(ID3D12Device* d, UINT pass, UINT obj);
    ComPtr<ID3D12CommandAllocator>             CmdListAlloc;
    std::unique_ptr<UploadBuffer<CBPerPass>>   PassCB;
    std::unique_ptr<UploadBuffer<CBPerObject>> ObjectCB;
    UINT64 Fence=0;
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

    void BuildRootSignature();
    void BuildDescriptorHeaps();
    void BuildConstantBufferViews();
    void BuildTextureViews();
    void BuildShadersAndInputLayout();
    void BuildDefaultCube();
    void BuildPSO();
    void BuildFrameResources();

    // Текстуры
    bool LoadTexture(const std::string& name, const std::wstring& path);
    void CreateProceduralTexture(const std::string& name);

    // OBJ+MTL
    bool LoadScene(const std::string& objFile);
    std::unordered_map<std::string,ObjMaterial> LoadMtl(const std::string& path);

    void UpdateObjectCBs(const GameTimer& gt);
    void UpdatePassCB   (const GameTimer& gt);
    void DrawRenderItems(ID3D12GraphicsCommandList* cmd);

    // Camera
    float mTheta=1.3f*XM_PI, mPhi=0.4f*XM_PI, mRadius=8.f;
    POINT mLastMousePos{};

    // Frame resources
    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource=nullptr;
    int mCurrFrameResourceIndex=0;

    // Pipeline
    ComPtr<ID3D12DescriptorHeap> mCbvSrvHeap;
    ComPtr<ID3D12RootSignature>  mRootSignature;
    ComPtr<ID3D12PipelineState>  mPSO;
    ComPtr<ID3DBlob> mVsByteCode, mPsByteCode;
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    // Текстуры: имя → ресурс
    std::unordered_map<std::string, ComPtr<ID3D12Resource>> mTextures;
    std::unordered_map<std::string, ComPtr<ID3D12Resource>> mTextureUploads;
    std::unordered_map<std::string, UINT>                   mTextureSrvIndex;
    std::string mFallbackTexName = "__checkerboard__";

    // Geometry & items
    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
    std::vector<std::unique_ptr<RenderItem>> mAllRItems;
    std::vector<RenderItem*>                 mOpaqueRItems;

    CBPerPass mMainPassCB{};
    UINT      mPassCbvOffset = 0;
    UINT      mSrvBaseOffset = 0;
    float     mUVOffset = 0.f;
    float     mTime     = 0.f;
};
