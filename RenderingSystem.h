#pragma once
// RenderingSystem.h — управляет двухпроходным deferred rendering
//
// Geometry Pass  : рендерит сцену в G-Buffer (Position / Normal / Albedo)
// Lighting Pass  : полноэкранный квад читает G-Buffer и считает освещение
//                  от трёх типов источников: Directional, Point, Spot

#include "d3dApp.h"
#include "GBuffer.h"
#include "UploadBuffer.h"

using Microsoft::WRL::ComPtr;

// ─── Структуры источников света ───────────────────────────────────────────────

struct DirectionalLight
{
    DirectX::XMFLOAT3 Direction; float Pad0 = 0;
    DirectX::XMFLOAT4 Ambient;
    DirectX::XMFLOAT4 Diffuse;
    DirectX::XMFLOAT4 Specular;
};

struct PointLight
{
    DirectX::XMFLOAT3 Position;    float Range;
    DirectX::XMFLOAT4 Ambient;
    DirectX::XMFLOAT4 Diffuse;
    DirectX::XMFLOAT4 Specular;
    DirectX::XMFLOAT3 Attenuation; float Pad1 = 0;  // const, linear, quad
};

struct SpotLight
{
    DirectX::XMFLOAT3 Position;    float Range;
    DirectX::XMFLOAT3 Direction;   float SpotPower;
    DirectX::XMFLOAT4 Ambient;
    DirectX::XMFLOAT4 Diffuse;
    DirectX::XMFLOAT4 Specular;
    DirectX::XMFLOAT3 Attenuation; float Pad2 = 0;
};

// ─── Constant buffer для lighting pass ────────────────────────────────────────
struct CBLighting
{
    DirectionalLight DirLight;
    PointLight       PointLights[3];
    SpotLight        Spot;
    DirectX::XMFLOAT3 EyePosW; float Pad0 = 0;
    int  NumPointLights = 3;
    int  HasSpot        = 1;
    DirectX::XMFLOAT2 Pad1;
};

// ─── RenderingSystem ──────────────────────────────────────────────────────────
class RenderingSystem
{
public:
    RenderingSystem() = default;

    // Инициализация: строит root signatures, PSO, fullscreen quad.
    // Вызывать после создания device, до первого кадра.
    void Initialize(ID3D12Device*              device,
                    ID3D12GraphicsCommandList* cmdList,
                    DXGI_FORMAT                backBufferFmt,
                    DXGI_FORMAT                depthFmt,
                    UINT                       cbvSrvUavDescSize,
                    UINT                       rtvDescSize);

    // Пересоздаёт G-Buffer при ресайзе.
    void OnResize(UINT width, UINT height,
                  ID3D12DescriptorHeap* rtvHeap,
                  ID3D12DescriptorHeap* srvHeap,
                  UINT                  srvBaseIndex);

    // Обновляет lighting CB (вызывать каждый кадр).
    void UpdateLightingCB(int frameIndex, const CBLighting& data);

    // Возвращает смещение lighting CBV в общей куче (3 слота × frameCount).
    UINT LightingCbvOffset() const { return mLightingCbvOffset; }

    // ── Passes ────────────────────────────────────────────────────────────────

    // Geometry pass: устанавливает PSO, RS, G-Buffer как RT и рисует переданные
    // объекты через callback.
    // drawCallback(cmd) — должен вызвать DrawIndexedInstanced для каждого объекта.
    void BeginGeometryPass(ID3D12GraphicsCommandList* cmd,
                           D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                           const D3D12_VIEWPORT& vp,
                           const D3D12_RECT&     sr);

    void EndGeometryPass(ID3D12GraphicsCommandList* cmd);

    // Lighting pass: полноэкранный квад → back buffer.
    void LightingPass(ID3D12GraphicsCommandList*  cmd,
                      D3D12_CPU_DESCRIPTOR_HANDLE  rtvBackBuffer,
                      D3D12_GPU_DESCRIPTOR_HANDLE  lightingCbvGpu,
                      const D3D12_VIEWPORT&        vp,
                      const D3D12_RECT&            sr);

    // Root signatures (нужны PhongApp для биндинга своих CBV)
    ID3D12RootSignature* GeometryRS()  const { return mGeometryRS.Get(); }
    ID3D12RootSignature* LightingRS()  const { return mLightingRS.Get(); }
    ID3D12PipelineState* GeometryPSO() const { return mGeometryPSO.Get(); }

    // Upload buffers для lighting CB (по одному на frame resource)
    UploadBuffer<CBLighting>* LightingCB(int frameIndex)
    { return mLightingCBs[frameIndex].get(); }

    // Создаёт CBV для lighting в общей куче.
    // Вызвать один раз после BuildDescriptorHeaps в PhongApp.
    void BuildLightingCBVs(ID3D12Device*         device,
                           ID3D12DescriptorHeap* heap,
                           UINT                  baseIndex,
                           UINT                  descSize);

    GBuffer& GetGBuffer() { return mGBuffer; }

private:
    void BuildRootSignatures(ID3D12Device* device);
    void BuildPSOs(ID3D12Device* device, DXGI_FORMAT backFmt, DXGI_FORMAT depthFmt);
    void BuildFullscreenQuad(ID3D12Device* device, ID3D12GraphicsCommandList* cmd);

    // G-Buffer
    GBuffer mGBuffer;

    // Descriptor sizes
    UINT mCbvSrvUavDescSize = 0;
    UINT mRtvDescSize       = 0;

    // Lighting CB offset in main heap
    UINT mLightingCbvOffset = 0;

    // Root signatures
    ComPtr<ID3D12RootSignature> mGeometryRS;
    ComPtr<ID3D12RootSignature> mLightingRS;

    // PSOs
    ComPtr<ID3D12PipelineState> mGeometryPSO;
    ComPtr<ID3D12PipelineState> mLightingPSO;

    // Fullscreen quad geometry
    struct QuadVertex { DirectX::XMFLOAT3 Pos; DirectX::XMFLOAT3 Normal; DirectX::XMFLOAT2 Tex; };
    ComPtr<ID3D12Resource> mQuadVB, mQuadIB;
    ComPtr<ID3D12Resource> mQuadVBUpload, mQuadIBUpload;

    // Lighting upload buffers (one per frame resource)
    std::unique_ptr<UploadBuffer<CBLighting>> mLightingCBs[3];

    // Shader bytecode
    ComPtr<ID3DBlob> mGeomVS, mGeomPS;
    ComPtr<ID3DBlob> mLightVS, mLightPS;

    // Input layout (shared with geometry pass)
    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

    DXGI_FORMAT mBackBufferFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthFmt      = DXGI_FORMAT_D24_UNORM_S8_UINT;
};
