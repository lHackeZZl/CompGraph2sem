// RenderingSystem.cpp
#include "RenderingSystem.h"
#include <cassert>
#include <algorithm>

using namespace DirectX;

// ─── Initialize ───────────────────────────────────────────────────────────────
void RenderingSystem::Initialize(ID3D12Device*              device,
                                 ID3D12GraphicsCommandList* cmdList,
                                 DXGI_FORMAT                backBufferFmt,
                                 DXGI_FORMAT                depthFmt,
                                 UINT                       cbvSrvUavDescSize,
                                 UINT                       rtvDescSize)
{
    mCbvSrvUavDescSize = cbvSrvUavDescSize;
    mRtvDescSize       = rtvDescSize;
    mBackBufferFmt     = backBufferFmt;
    mDepthFmt          = depthFmt;

    // Upload buffers for lighting CB (3 frame resources)
    for (int i = 0; i < 3; ++i)
    {
        mLightingCBs[i]  = std::make_unique<UploadBuffer<CBLighting>>(device, 1, true);
        mPointLightSBs[i] = std::make_unique<UploadBuffer<PointLight>>(device, MaxPointLights, false);
    }

    BuildRootSignatures(device);
    BuildPSOs(device, backBufferFmt, depthFmt);
    BuildFullscreenQuad(device, cmdList);
}

// ─── OnResize ─────────────────────────────────────────────────────────────────
void RenderingSystem::OnResize(UINT                  width,
                               UINT                  height,
                               ID3D12DescriptorHeap* rtvHeap,
                               ID3D12DescriptorHeap* srvHeap,
                               UINT                  srvBaseIndex)
{
    mGBuffer.Create(
        /* device set in Create, pass via stored ptr — we re-use heap ptrs */
        // NOTE: device pointer is passed through GBuffer::Create directly.
        // Here we just call Release; actual Create call is in PhongApp::OnResize
        // after it gets the device pointer.
        // This method is a thin wrapper — see PhongApp for full call.
        nullptr, width, height,
        rtvHeap, mRtvDescSize,
        srvHeap, srvBaseIndex,
        mCbvSrvUavDescSize);
}

// ─── Lighting CB views ────────────────────────────────────────────────────────
void RenderingSystem::BuildLightingViews(ID3D12Device*         device,
                                         ID3D12DescriptorHeap* heap,
                                         UINT                  cbvBaseIndex,
                                         UINT                  pointSrvBaseIndex,
                                         UINT                  descSize)
{
    mLightingCbvOffset  = cbvBaseIndex;
    mPointLightSrvOffset = pointSrvBaseIndex;

    UINT sz = d3dUtil::CalcConstantBufferByteSize(sizeof(CBLighting));
    for (int i = 0; i < 3; ++i)
    {
        auto cbvH = CD3DX12_CPU_HANDLE(
            heap->GetCPUDescriptorHandleForHeapStart(),
            cbvBaseIndex + i, descSize);
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {
            mLightingCBs[i]->Resource()->GetGPUVirtualAddress(), sz };
        device->CreateConstantBufferView(&cbv, cbvH);

        auto srvH = CD3DX12_CPU_HANDLE(
            heap->GetCPUDescriptorHandleForHeapStart(),
            pointSrvBaseIndex + i, descSize);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format                  = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.FirstElement     = 0;
        srv.Buffer.NumElements      = MaxPointLights;
        srv.Buffer.StructureByteStride = sizeof(PointLight);
        srv.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_NONE;
        device->CreateShaderResourceView(mPointLightSBs[i]->Resource(), &srv, srvH);
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE RenderingSystem::PointLightsSrvGpuHandle(
    ID3D12DescriptorHeap* heap, int frameIndex, UINT descSize) const
{
    return CD3DX12_GPU_HANDLE(heap->GetGPUDescriptorHandleForHeapStart(),
        mPointLightSrvOffset + frameIndex, descSize);
}

void RenderingSystem::UpdateLightingData(int frameIndex, const CBLighting& data,
                                         const std::vector<PointLight>& pointLights)
{
    CBLighting copy = data;
    copy.NumPointLights = (int)std::min<size_t>(pointLights.size(), MaxPointLights);
    mLightingCBs[frameIndex]->CopyData(0, copy);

    for (int i = 0; i < copy.NumPointLights; ++i)
        mPointLightSBs[frameIndex]->CopyData(i, pointLights[(size_t)i]);
}

// ─── Root Signatures ──────────────────────────────────────────────────────────
void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
    // ── Geometry RS ───────────────────────────────────────────────────────────
    // slot 0 : CBV table (b0) — per-object
    // slot 1 : CBV table (b1) — per-pass
    // slot 2 : SRV table (t0) — diffuse texture
    {
        D3D12_DESCRIPTOR_RANGE r0 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        D3D12_DESCRIPTOR_RANGE r1 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);
        D3D12_DESCRIPTOR_RANGE r2 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

        D3D12_ROOT_PARAMETER p[3];
        p[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r0);
        p[1] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r1);
        p[2] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r2, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters     = 3;
        desc.pParameters       = p;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> ser, err;
        HRESULT hr = D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(
            0, ser->GetBufferPointer(), ser->GetBufferSize(),
            IID_PPV_ARGS(&mGeometryRS)));
    }

    // ── Lighting RS ───────────────────────────────────────────────────────────
    // slot 0 : CBV table (b0) — lighting data (dir + point + spot)
    // slot 1 : SRV table (t0,t1,t2) — G-Buffer (Position, Normal, Albedo)
    {
        D3D12_DESCRIPTOR_RANGE r0 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        D3D12_DESCRIPTOR_RANGE r1 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0);
        D3D12_DESCRIPTOR_RANGE r2 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

        D3D12_ROOT_PARAMETER p[3];
        p[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r0);
        p[1] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r1, D3D12_SHADER_VISIBILITY_PIXEL);
        p[2] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r2, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp.AddressU = samp.AddressV = samp.AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters     = 3;
        desc.pParameters       = p;
        desc.NumStaticSamplers = 1;
        desc.pStaticSamplers   = &samp;
        desc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> ser, err;
        HRESULT hr = D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
        if (err) OutputDebugStringA((char*)err->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(
            0, ser->GetBufferPointer(), ser->GetBufferSize(),
            IID_PPV_ARGS(&mLightingRS)));
    }
}

// ─── PSOs ─────────────────────────────────────────────────────────────────────
void RenderingSystem::BuildPSOs(ID3D12Device* device,
                                DXGI_FORMAT   backFmt,
                                DXGI_FORMAT   depthFmt)
{
    mGeomVS  = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "VS", "vs_5_1");
    mGeomPS  = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "PS", "ps_5_1");
    mLightVS = d3dUtil::CompileShader(L"lighting.hlsl", nullptr, "VS", "vs_5_1");
    mLightPS = d3dUtil::CompileShader(L"lighting.hlsl", nullptr, "PS", "ps_5_1");

    mInputLayout = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0, 0,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,   0,24,
         D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };

    // ── Geometry PSO ──────────────────────────────────────────────────────────
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.InputLayout    = { mInputLayout.data(), (UINT)mInputLayout.size() };
        d.pRootSignature = mGeometryRS.Get();
        d.VS = { mGeomVS->GetBufferPointer(), mGeomVS->GetBufferSize() };
        d.PS = { mGeomPS->GetBufferPointer(), mGeomPS->GetBufferSize() };

        D3D12_RASTERIZER_DESC rast = CD3DX12_RASTERIZER_DESC_DEFAULT();
        rast.CullMode   = D3D12_CULL_MODE_NONE; // breakfast_room имеет грани в обе стороны
        d.RasterizerState   = rast;
        d.BlendState        = CD3DX12_BLEND_DESC_DEFAULT();
        d.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
        d.SampleMask        = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets  = kGBufferCount;
        for (UINT i = 0; i < kGBufferCount; ++i)
            d.RTVFormats[i] = kGBufferFormats[i];
        d.SampleDesc.Count = 1;
        d.DSVFormat        = depthFmt;
        ThrowIfFailed(device->CreateGraphicsPipelineState(
            &d, IID_PPV_ARGS(&mGeometryPSO)));
    }

    // ── Lighting PSO (fullscreen quad, no depth) ───────────────────────────
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.InputLayout    = { mInputLayout.data(), (UINT)mInputLayout.size() };
        d.pRootSignature = mLightingRS.Get();
        d.VS = { mLightVS->GetBufferPointer(), mLightVS->GetBufferSize() };
        d.PS = { mLightPS->GetBufferPointer(), mLightPS->GetBufferSize() };

        d.RasterizerState = CD3DX12_RASTERIZER_DESC_DEFAULT();
        d.BlendState      = CD3DX12_BLEND_DESC_DEFAULT();

        D3D12_DEPTH_STENCIL_DESC dsd = CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
        dsd.DepthEnable    = FALSE;
        dsd.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState = dsd;

        d.SampleMask        = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets  = 1;
        d.RTVFormats[0]     = backFmt;
        d.SampleDesc.Count  = 1;
        d.DSVFormat         = depthFmt;
        ThrowIfFailed(device->CreateGraphicsPipelineState(
            &d, IID_PPV_ARGS(&mLightingPSO)));
    }
}

// ─── Fullscreen Quad ──────────────────────────────────────────────────────────
void RenderingSystem::BuildFullscreenQuad(ID3D12Device*              device,
                                          ID3D12GraphicsCommandList* cmd)
{
    // NDC quad covering entire screen
    QuadVertex verts[4] = {
        { {-1.f,  1.f, 0.f}, {0,0,0}, {0.f, 0.f} },
        { { 1.f,  1.f, 0.f}, {0,0,0}, {1.f, 0.f} },
        { {-1.f, -1.f, 0.f}, {0,0,0}, {0.f, 1.f} },
        { { 1.f, -1.f, 0.f}, {0,0,0}, {1.f, 1.f} },
    };
    uint32_t idx[6] = { 0,1,2, 1,3,2 };

    UINT64 vbSz = sizeof(verts);
    UINT64 ibSz = sizeof(idx);

    auto defHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uplHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    // VB
    {
        auto desc = CD3DX12_RESOURCE_DESC_BUFFER(vbSz);
        ThrowIfFailed(device->CreateCommittedResource(
            &defHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mQuadVB)));
        ThrowIfFailed(device->CreateCommittedResource(
            &uplHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mQuadVBUpload)));

        BYTE* p = nullptr;
        mQuadVBUpload->Map(0, nullptr, (void**)&p);
        memcpy(p, verts, (size_t)vbSz);
        mQuadVBUpload->Unmap(0, nullptr);

        auto b1 = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mQuadVB.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b1);
        cmd->CopyBufferRegion(mQuadVB.Get(), 0, mQuadVBUpload.Get(), 0, vbSz);
        auto b2 = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mQuadVB.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd->ResourceBarrier(1, &b2);
    }
    // IB
    {
        auto desc = CD3DX12_RESOURCE_DESC_BUFFER(ibSz);
        ThrowIfFailed(device->CreateCommittedResource(
            &defHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mQuadIB)));
        ThrowIfFailed(device->CreateCommittedResource(
            &uplHeap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&mQuadIBUpload)));

        BYTE* p = nullptr;
        mQuadIBUpload->Map(0, nullptr, (void**)&p);
        memcpy(p, idx, (size_t)ibSz);
        mQuadIBUpload->Unmap(0, nullptr);

        auto b1 = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mQuadIB.Get(), D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->ResourceBarrier(1, &b1);
        cmd->CopyBufferRegion(mQuadIB.Get(), 0, mQuadIBUpload.Get(), 0, ibSz);
        auto b2 = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mQuadIB.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd->ResourceBarrier(1, &b2);
    }
}

// ─── Passes ───────────────────────────────────────────────────────────────────
void RenderingSystem::BeginGeometryPass(ID3D12GraphicsCommandList* cmd,
                                        D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                                        const D3D12_VIEWPORT& vp,
                                        const D3D12_RECT&     sr)
{
    cmd->SetPipelineState(mGeometryPSO.Get());
    cmd->SetGraphicsRootSignature(mGeometryRS.Get());
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);
    mGBuffer.BeginGeometryPass(cmd, dsv, mRtvDescSize);
}

void RenderingSystem::EndGeometryPass(ID3D12GraphicsCommandList* cmd)
{
    mGBuffer.EndGeometryPass(cmd);
}

void RenderingSystem::LightingPass(ID3D12GraphicsCommandList*  cmd,
                                   D3D12_CPU_DESCRIPTOR_HANDLE  rtvBackBuffer,
                                   D3D12_GPU_DESCRIPTOR_HANDLE  lightingCbvGpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE  pointLightsSrvGpu,
                                   const D3D12_VIEWPORT&        vp,
                                   const D3D12_RECT&            sr)
{
    cmd->SetPipelineState(mLightingPSO.Get());
    cmd->SetGraphicsRootSignature(mLightingRS.Get());
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sr);

    const float cc[4] = { 0.08f, 0.08f, 0.12f, 1.f };
    cmd->ClearRenderTargetView(rtvBackBuffer, cc, 0, nullptr);
    cmd->OMSetRenderTargets(1, &rtvBackBuffer, true, nullptr);

    // slot 0 — lighting CBV
    cmd->SetGraphicsRootDescriptorTable(0, lightingCbvGpu);
    // slot 1 — G-Buffer SRVs (Position, Normal, Albedo — три подряд)
    cmd->SetGraphicsRootDescriptorTable(1, mGBuffer.FirstSrvGpuHandle());
    // slot 2 — StructuredBuffer<PointLight>
    cmd->SetGraphicsRootDescriptorTable(2, pointLightsSrvGpu);

    // Fullscreen quad
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = mQuadVB->GetGPUVirtualAddress();
    vbv.SizeInBytes    = sizeof(QuadVertex) * 4;
    vbv.StrideInBytes  = sizeof(QuadVertex);

    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = mQuadIB->GetGPUVirtualAddress();
    ibv.SizeInBytes    = sizeof(uint32_t) * 6;
    ibv.Format         = DXGI_FORMAT_R32_UINT;

    cmd->IASetVertexBuffers(0, 1, &vbv);
    cmd->IASetIndexBuffer(&ibv);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawIndexedInstanced(6, 1, 0, 0, 0);
}
