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

    BuildShadowResources(device);
    BuildRootSignatures(device);
    BuildPSOs(device, backBufferFmt, depthFmt);
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

void RenderingSystem::BuildShadowResources(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = CascadeCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(device->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&mShadowDsvHeap)));

    auto heap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto texture = CD3DX12_RESOURCE_DESC_TEX2D(
        DXGI_FORMAT_R32_TYPELESS, ShadowMapSize, ShadowMapSize,
        (UINT16)CascadeCount, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.f;
    ThrowIfFailed(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &texture,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&mShadowMap)));

    UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    for (UINT cascade = 0; cascade < CascadeCount; ++cascade)
    {
        mShadowDsv[cascade] = CD3DX12_CPU_HANDLE(
            mShadowDsvHeap->GetCPUDescriptorHandleForHeapStart(), cascade, dsvSize);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
        dsv.Format = DXGI_FORMAT_D32_FLOAT;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Texture2DArray.FirstArraySlice = cascade;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.MipSlice = 0;
        device->CreateDepthStencilView(mShadowMap.Get(), &dsv, mShadowDsv[cascade]);
    }

    mShadowViewport = { 0.f, 0.f, (float)ShadowMapSize, (float)ShadowMapSize, 0.f, 1.f };
    mShadowScissor = { 0, 0, (LONG)ShadowMapSize, (LONG)ShadowMapSize };
    mShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
}

void RenderingSystem::BuildShadowView(ID3D12Device* device,
                                      ID3D12DescriptorHeap* srvHeap,
                                      UINT srvIndex, UINT descriptorSize)
{
    auto cpu = CD3DX12_CPU_HANDLE(
        srvHeap->GetCPUDescriptorHandleForHeapStart(), srvIndex, descriptorSize);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srv.Texture2DArray.MostDetailedMip = 0;
    srv.Texture2DArray.MipLevels = 1;
    srv.Texture2DArray.FirstArraySlice = 0;
    srv.Texture2DArray.ArraySize = CascadeCount;
    device->CreateShaderResourceView(mShadowMap.Get(), &srv, cpu);
    mShadowSrvGpu = CD3DX12_GPU_HANDLE(
        srvHeap->GetGPUDescriptorHandleForHeapStart(), srvIndex, descriptorSize);
}

// ─── Root Signatures ──────────────────────────────────────────────────────────
void RenderingSystem::BuildRootSignatures(ID3D12Device* device)
{
    // ── Geometry RS ───────────────────────────────────────────────────────────
    // slot 0 : CBV table (b0) — per-object
    // slot 1 : CBV table (b1) — per-pass
    // slot 2 : SRV table (t0) — diffuse texture
    // slot 3 : SRV table (t1) — normal map
    // slot 4 : SRV table (t2) — displacement/height map
    {
        D3D12_DESCRIPTOR_RANGE r0 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        D3D12_DESCRIPTOR_RANGE r1 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);
        D3D12_DESCRIPTOR_RANGE r2 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        D3D12_DESCRIPTOR_RANGE r3 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
        D3D12_DESCRIPTOR_RANGE r4 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

        D3D12_ROOT_PARAMETER p[5];
        p[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r0);
        p[1] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r1);
        p[2] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r2, D3D12_SHADER_VISIBILITY_PIXEL);
        p[3] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r3, D3D12_SHADER_VISIBILITY_PIXEL);
        p[4] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r4, D3D12_SHADER_VISIBILITY_ALL);

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters     = 5;
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

    // ── Shadow RS ────────────────────────────────────────────────────────────
    // slot 0: per-object CBV table (b0), slot 1: 4x4 light matrix as constants.
    {
        D3D12_DESCRIPTOR_RANGE objectRange =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        D3D12_ROOT_PARAMETER p[2];
        p[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &objectRange, D3D12_SHADER_VISIBILITY_VERTEX);
        p[1] = {};
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[1].Constants.Num32BitValues = 16;
        p[1].Constants.ShaderRegister = 1;
        p[1].Constants.RegisterSpace = 0;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = 2;
        desc.pParameters = p;
        desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> serialized, errors;
        HRESULT hr = D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&mShadowRS)));
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
        D3D12_DESCRIPTOR_RANGE r3 =
            CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);

        D3D12_ROOT_PARAMETER p[4];
        p[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r0);
        p[1] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r1, D3D12_SHADER_VISIBILITY_PIXEL);
        p[2] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r2, D3D12_SHADER_VISIBILITY_PIXEL);
        p[3] = CD3DX12_ROOT_PARAMETER_TABLE(1, &r3, D3D12_SHADER_VISIBILITY_PIXEL);

        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].MaxLOD           = D3D12_FLOAT32_MAX;
        samplers[0].ComparisonFunc   = D3D12_COMPARISON_FUNC_ALWAYS;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        samplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[1].ShaderRegister = 1;
        samplers[1].RegisterSpace = 0;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters     = 4;
        desc.pParameters       = p;
        desc.NumStaticSamplers = 2;
        desc.pStaticSamplers   = samplers;
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
    mGeomVS     = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "VS",     "vs_5_1");
    mGeomTessVS = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "VSTess", "vs_5_1");
    mGeomHS     = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "HS",     "hs_5_1");
    mGeomDS     = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "DS",     "ds_5_1");
    mGeomPS     = d3dUtil::CompileShader(L"gbuffer.hlsl",  nullptr, "PS",     "ps_5_1");
    mLightVS    = d3dUtil::CompileShader(L"lighting.hlsl", nullptr, "VS",     "vs_5_1");
    mLightPS    = d3dUtil::CompileShader(L"lighting.hlsl", nullptr, "PS",     "ps_5_1");
    mShadowVS   = d3dUtil::CompileShader(L"shadow.hlsl",   nullptr, "VS",     "vs_5_1");

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

    // ── Geometry Tessellation PSO ─────────────────────────────────────────────
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.InputLayout    = { mInputLayout.data(), (UINT)mInputLayout.size() };
        d.pRootSignature = mGeometryRS.Get();
        d.VS = { mGeomTessVS->GetBufferPointer(), mGeomTessVS->GetBufferSize() };
        d.HS = { mGeomHS->GetBufferPointer(),     mGeomHS->GetBufferSize() };
        d.DS = { mGeomDS->GetBufferPointer(),     mGeomDS->GetBufferSize() };
        d.PS = { mGeomPS->GetBufferPointer(),     mGeomPS->GetBufferSize() };

        D3D12_RASTERIZER_DESC rast = CD3DX12_RASTERIZER_DESC_DEFAULT();
        rast.CullMode = D3D12_CULL_MODE_NONE;
        d.RasterizerState   = rast;
        d.BlendState        = CD3DX12_BLEND_DESC_DEFAULT();
        d.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
        d.SampleMask        = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        d.NumRenderTargets  = kGBufferCount;
        for (UINT i = 0; i < kGBufferCount; ++i)
            d.RTVFormats[i] = kGBufferFormats[i];
        d.SampleDesc.Count = 1;
        d.DSVFormat        = depthFmt;
        ThrowIfFailed(device->CreateGraphicsPipelineState(
            &d, IID_PPV_ARGS(&mGeometryTessPSO)));
    }

    // ── Lighting PSO (fullscreen quad, no depth) ───────────────────────────
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        // Full-screen geometry is generated from SV_VertexID: no input layout,
        // vertex buffer or index buffer is needed for the lighting/post pass.
        d.InputLayout    = { nullptr, 0 };
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

    // ── Cascaded shadow depth-only PSO ───────────────────────────────────────
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
        d.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
        d.pRootSignature = mShadowRS.Get();
        d.VS = { mShadowVS->GetBufferPointer(), mShadowVS->GetBufferSize() };
        D3D12_RASTERIZER_DESC rasterizer = CD3DX12_RASTERIZER_DESC_DEFAULT();
        rasterizer.CullMode = D3D12_CULL_MODE_NONE;
        rasterizer.DepthBias = 1200;
        rasterizer.SlopeScaledDepthBias = 1.5f;
        rasterizer.DepthBiasClamp = 0.01f;
        d.RasterizerState = rasterizer;
        d.BlendState = CD3DX12_BLEND_DESC_DEFAULT();
        d.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
        d.SampleMask = UINT_MAX;
        d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        d.NumRenderTargets = 0;
        d.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        d.SampleDesc.Count = 1;
        ThrowIfFailed(device->CreateGraphicsPipelineState(
            &d, IID_PPV_ARGS(&mShadowPSO)));
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

void RenderingSystem::BeginShadowPass(ID3D12GraphicsCommandList* cmd)
{
    if (mShadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mShadowMap.Get(), mShadowMapState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        cmd->ResourceBarrier(1, &barrier);
        mShadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }
    cmd->SetPipelineState(mShadowPSO.Get());
    cmd->SetGraphicsRootSignature(mShadowRS.Get());
    cmd->RSSetViewports(1, &mShadowViewport);
    cmd->RSSetScissorRects(1, &mShadowScissor);
}

void RenderingSystem::BeginShadowCascade(ID3D12GraphicsCommandList* cmd,
                                         UINT cascadeIndex,
                                         const XMFLOAT4X4& lightViewProj)
{
    assert(cascadeIndex < CascadeCount);
    cmd->ClearDepthStencilView(mShadowDsv[cascadeIndex],
        D3D12_CLEAR_FLAG_DEPTH, 1.f, 0, 0, nullptr);
    cmd->OMSetRenderTargets(0, nullptr, FALSE, &mShadowDsv[cascadeIndex]);
    cmd->SetGraphicsRoot32BitConstants(1, 16, &lightViewProj, 0);
}

void RenderingSystem::EndShadowPass(ID3D12GraphicsCommandList* cmd)
{
    auto barrier = CD3DX12_RESOURCE_BARRIER_TRANSITION(
        mShadowMap.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->ResourceBarrier(1, &barrier);
    mShadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}

void RenderingSystem::LightingPass(ID3D12GraphicsCommandList*  cmd,
                                   D3D12_CPU_DESCRIPTOR_HANDLE  rtvBackBuffer,
                                   D3D12_GPU_DESCRIPTOR_HANDLE  lightingCbvGpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE  pointLightsSrvGpu,
                                   D3D12_GPU_DESCRIPTOR_HANDLE  shadowMapSrvGpu,
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
    // slot 3 — four-slice cascaded shadow map
    cmd->SetGraphicsRootDescriptorTable(3, shadowMapSrvGpu);

    // Six vertices are synthesized in VS from SV_VertexID (two triangles).
    cmd->IASetVertexBuffers(0, 0, nullptr);
    cmd->IASetIndexBuffer(nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(6, 1, 0, 0);
}
