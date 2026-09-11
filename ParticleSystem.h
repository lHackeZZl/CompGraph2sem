#pragma once
#include "d3dApp.h"

class ParticleSystem
{
public:
    static constexpr UINT MaxParticles = 2048;

    void Initialize(ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
                    DXGI_FORMAT depthFormat);
    void Update(ID3D12GraphicsCommandList* commandList, float deltaTime, float totalTime);
    void Render(ID3D12GraphicsCommandList* commandList,
                DirectX::FXMMATRIX viewProjection,
                DirectX::FXMVECTOR cameraRight,
                DirectX::FXMVECTOR cameraUp,
                DirectX::FXMVECTOR cameraForward);

private:
    struct Particle
    {
        DirectX::XMFLOAT3 Position; float Age;
        DirectX::XMFLOAT3 Velocity; float Lifetime;
        DirectX::XMFLOAT4 Color;
        float Size; DirectX::XMFLOAT3 Padding;
    };

    struct RenderConstants
    {
        DirectX::XMFLOAT4X4 ViewProjection;
        DirectX::XMFLOAT3 CameraRight; float ParticleScale = 1.f;
        DirectX::XMFLOAT3 CameraUp; float Pad0 = 0.f;
        DirectX::XMFLOAT3 CameraForward; float Pad1 = 0.f;
    };

    void BuildResources(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
    void BuildDescriptors(ID3D12Device* device);
    void BuildRootSignatures(ID3D12Device* device);
    void BuildPipelineStates(ID3D12Device* device, DXGI_FORMAT depthFormat);

    Microsoft::WRL::ComPtr<ID3D12Resource> mParticleBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounterBuffers[2];
    Microsoft::WRL::ComPtr<ID3D12Resource> mInitialUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> mCounterUpload;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mComputeRootSignature;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> mRenderRootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mComputePSO;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> mRenderPSO;
    Microsoft::WRL::ComPtr<ID3DBlob> mComputeShader;
    Microsoft::WRL::ComPtr<ID3DBlob> mVertexShader;
    Microsoft::WRL::ComPtr<ID3DBlob> mGeometryShader;
    Microsoft::WRL::ComPtr<ID3DBlob> mPixelShader;

    UINT mDescriptorSize = 0;
    UINT mActiveBuffer = 0;
    D3D12_RESOURCE_STATES mParticleStates[2] = {
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
};
