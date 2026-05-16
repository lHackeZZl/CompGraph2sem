// GBuffer.cpp
#include "GBuffer.h"
#include "d3dApp.h"   // ThrowIfFailed

void GBuffer::Create(ID3D12Device*         device,
                     UINT                  width,
                     UINT                  height,
                     ID3D12DescriptorHeap* rtvHeap,
                     UINT                  rtvDescSize,
                     ID3D12DescriptorHeap* srvHeap,
                     UINT                  srvBaseIndex,
                     UINT                  cbvSrvUavDescSize)
{
    Release();
    mWidth  = width;
    mHeight = height;

    auto defHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    for (UINT i = 0; i < kGBufferCount; ++i)
    {
        // ── RT resource ──────────────────────────────────────────────────────
        D3D12_RESOURCE_DESC td = CD3DX12_RESOURCE_DESC_TEX2D(
            kGBufferFormats[i], width, height,
            1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

        float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = kGBufferFormats[i];
        memcpy(cv.Color, clear, sizeof(clear));

        ThrowIfFailed(device->CreateCommittedResource(
            &defHeap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_PPV_ARGS(&mRTs[i])));

        // ── RTV ──────────────────────────────────────────────────────────────
        mRtvCpu[i] = CD3DX12_CPU_HANDLE(
            rtvHeap->GetCPUDescriptorHandleForHeapStart(), i, rtvDescSize);

        D3D12_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format        = kGBufferFormats[i];
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(mRTs[i].Get(), &rd, mRtvCpu[i]);

        // ── SRV ──────────────────────────────────────────────────────────────
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format                  = kGBufferFormats[i];
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels     = 1;

        auto srvCpu = CD3DX12_CPU_HANDLE(
            srvHeap->GetCPUDescriptorHandleForHeapStart(),
            srvBaseIndex + i, cbvSrvUavDescSize);
        device->CreateShaderResourceView(mRTs[i].Get(), &sd, srvCpu);
    }

    // GPU-хэндл первого SRV (три идут подряд)
    mFirstSrvGpu = CD3DX12_GPU_HANDLE(
        srvHeap->GetGPUDescriptorHandleForHeapStart(),
        srvBaseIndex, cbvSrvUavDescSize);
}

void GBuffer::Release()
{
    for (UINT i = 0; i < kGBufferCount; ++i)
        mRTs[i] = nullptr;
}

void GBuffer::BeginGeometryPass(ID3D12GraphicsCommandList* cmd,
                                D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                UINT /*rtvDescSize*/)
{
    // PIXEL_SHADER_RESOURCE → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barriers[kGBufferCount];
    for (UINT i = 0; i < kGBufferCount; ++i)
        barriers[i] = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mRTs[i].Get(),
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_RENDER_TARGET);
    cmd->ResourceBarrier(kGBufferCount, barriers);

    float clearColor[4] = { 0.f, 0.f, 0.f, 0.f };
    for (UINT i = 0; i < kGBufferCount; ++i)
        cmd->ClearRenderTargetView(mRtvCpu[i], clearColor, 0, nullptr);

    cmd->ClearDepthStencilView(dsv,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);

    cmd->OMSetRenderTargets(kGBufferCount, mRtvCpu, FALSE, &dsv);
}

void GBuffer::EndGeometryPass(ID3D12GraphicsCommandList* cmd)
{
    // RENDER_TARGET → PIXEL_SHADER_RESOURCE
    D3D12_RESOURCE_BARRIER barriers[kGBufferCount];
    for (UINT i = 0; i < kGBufferCount; ++i)
        barriers[i] = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mRTs[i].Get(),
            D3D12_RESOURCE_STATE_RENDER_TARGET,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(kGBufferCount, barriers);
}
