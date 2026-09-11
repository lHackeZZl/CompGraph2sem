#include "ParticleSystem.h"
#include "GBuffer.h"
#include <vector>
#include <cmath>
#include <algorithm>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

void ParticleSystem::Initialize(ID3D12Device* device,
                                ID3D12GraphicsCommandList* commandList,
                                DXGI_FORMAT depthFormat)
{
    BuildResources(device, commandList);
    BuildDescriptors(device);
    BuildRootSignatures(device);
    BuildPipelineStates(device, depthFormat);
}

void ParticleSystem::BuildResources(ID3D12Device* device,
                                    ID3D12GraphicsCommandList* commandList)
{
    std::vector<Particle> particles(MaxParticles);
    uint32_t randomState = 0x51A7E123u;
    auto random01 = [&]()
    {
        randomState = randomState * 1664525u + 1013904223u;
        return (float)(randomState & 0x00ffffffu) / 16777215.0f;
    };

    const XMFLOAT3 emitter = { -0.6f, -0.9f, 2.7f };
    for (UINT i = 0; i < MaxParticles; ++i)
    {
        float angle = random01() * XM_2PI;
        float radial = 0.25f + random01() * 1.15f;
        float speed = 2.8f + random01() * 3.8f;
        Particle& p = particles[i];
        p.Position = emitter;
        p.Velocity = { cosf(angle) * radial, speed, sinf(angle) * radial };
        p.Lifetime = 1.8f + random01() * 2.4f;
        p.Age = random01() * p.Lifetime;
        p.Position.x += p.Velocity.x * p.Age;
        p.Position.y += p.Velocity.y * p.Age - 1.9f * p.Age * p.Age;
        p.Position.z += p.Velocity.z * p.Age;
        float hue = random01();
        p.Color = { 1.f, 0.18f + hue * 0.67f, 0.03f + hue * 0.22f, 1.f };
        p.Size = 0.055f + random01() * 0.075f;
        p.Padding = { 0.f, 0.f, 0.f };
    }

    const UINT64 particleBytes = (UINT64)sizeof(Particle) * MaxParticles;
    auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto particleDesc = CD3DX12_RESOURCE_DESC_BUFFER(
        particleBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &particleDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&mParticleBuffers[0])));
    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &particleDesc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        IID_PPV_ARGS(&mParticleBuffers[1])));

    auto uploadDesc = CD3DX12_RESOURCE_DESC_BUFFER(particleBytes);
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&mInitialUpload)));
    BYTE* mappedParticles = nullptr;
    ThrowIfFailed(mInitialUpload->Map(0, nullptr, (void**)&mappedParticles));
    memcpy(mappedParticles, particles.data(), (size_t)particleBytes);
    mInitialUpload->Unmap(0, nullptr);
    commandList->CopyBufferRegion(
        mParticleBuffers[0].Get(), 0, mInitialUpload.Get(), 0, particleBytes);

    // Each Append/Consume UAV has its own hidden 32-bit counter resource.
    auto counterDesc = CD3DX12_RESOURCE_DESC_BUFFER(
        sizeof(UINT), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    for (UINT i = 0; i < 2; ++i)
    {
        ThrowIfFailed(device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &counterDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&mCounterBuffers[i])));
    }

    auto counterUploadDesc = CD3DX12_RESOURCE_DESC_BUFFER(256);
    ThrowIfFailed(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &counterUploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&mCounterUpload)));
    UINT* mappedCounters = nullptr;
    ThrowIfFailed(mCounterUpload->Map(0, nullptr, (void**)&mappedCounters));
    mappedCounters[0] = MaxParticles;
    mappedCounters[1] = 0;
    mCounterUpload->Unmap(0, nullptr);
    commandList->CopyBufferRegion(mCounterBuffers[0].Get(), 0, mCounterUpload.Get(), 0, 4);
    commandList->CopyBufferRegion(mCounterBuffers[1].Get(), 0, mCounterUpload.Get(), 4, 4);

    D3D12_RESOURCE_BARRIER barriers[3] = {
        CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mParticleBuffers[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mCounterBuffers[0].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mCounterBuffers[1].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    commandList->ResourceBarrier(3, barriers);
    mParticleStates[0] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    mParticleStates[1] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
}

void ParticleSystem::BuildDescriptors(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = 4; // UAV0, UAV1, SRV0, SRV1
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(device->CreateDescriptorHeap(
        &heapDesc, IID_PPV_ARGS(&mDescriptorHeap)));
    mDescriptorSize = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (UINT i = 0; i < 2; ++i)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = DXGI_FORMAT_UNKNOWN;
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Buffer.NumElements = MaxParticles;
        uav.Buffer.StructureByteStride = sizeof(Particle);
        uav.Buffer.CounterOffsetInBytes = 0;
        auto uavHandle = CD3DX12_CPU_HANDLE(
            mDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), i, mDescriptorSize);
        device->CreateUnorderedAccessView(
            mParticleBuffers[i].Get(), mCounterBuffers[i].Get(), &uav, uavHandle);

        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = MaxParticles;
        srv.Buffer.StructureByteStride = sizeof(Particle);
        auto srvHandle = CD3DX12_CPU_HANDLE(
            mDescriptorHeap->GetCPUDescriptorHandleForHeapStart(), 2 + i, mDescriptorSize);
        device->CreateShaderResourceView(mParticleBuffers[i].Get(), &srv, srvHandle);
    }
}

void ParticleSystem::BuildRootSignatures(ID3D12Device* device)
{
    auto serialize = [&](const D3D12_ROOT_SIGNATURE_DESC& desc,
                         ComPtr<ID3D12RootSignature>& rootSignature)
    {
        ComPtr<ID3DBlob> serialized, errors;
        HRESULT hr = D3D12SerializeRootSignature(
            &desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
        if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
        ThrowIfFailed(hr);
        ThrowIfFailed(device->CreateRootSignature(
            0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature)));
    };

    D3D12_DESCRIPTOR_RANGE inputRange =
        CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
    D3D12_DESCRIPTOR_RANGE outputRange =
        CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
    D3D12_ROOT_PARAMETER computeParameters[3] = {};
    computeParameters[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &inputRange);
    computeParameters[1] = CD3DX12_ROOT_PARAMETER_TABLE(1, &outputRange);
    computeParameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    computeParameters[2].Constants.Num32BitValues = 8;
    computeParameters[2].Constants.ShaderRegister = 0;
    computeParameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC computeDesc = {};
    computeDesc.NumParameters = 3;
    computeDesc.pParameters = computeParameters;
    serialize(computeDesc, mComputeRootSignature);

    D3D12_DESCRIPTOR_RANGE particleRange =
        CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    D3D12_ROOT_PARAMETER renderParameters[2] = {};
    renderParameters[0] = CD3DX12_ROOT_PARAMETER_TABLE(1, &particleRange);
    renderParameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    renderParameters[1].Constants.Num32BitValues = sizeof(RenderConstants) / 4;
    renderParameters[1].Constants.ShaderRegister = 0;
    renderParameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC renderDesc = {};
    renderDesc.NumParameters = 2;
    renderDesc.pParameters = renderParameters;
    renderDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    serialize(renderDesc, mRenderRootSignature);
}

void ParticleSystem::BuildPipelineStates(ID3D12Device* device, DXGI_FORMAT depthFormat)
{
    mComputeShader = d3dUtil::CompileShader(
        L"particles_update.hlsl", nullptr, "CS", "cs_5_1");
    mVertexShader = d3dUtil::CompileShader(
        L"particles_render.hlsl", nullptr, "VS", "vs_5_1");
    mGeometryShader = d3dUtil::CompileShader(
        L"particles_render.hlsl", nullptr, "GS", "gs_5_1");
    mPixelShader = d3dUtil::CompileShader(
        L"particles_render.hlsl", nullptr, "PS", "ps_5_1");

    D3D12_COMPUTE_PIPELINE_STATE_DESC compute = {};
    compute.pRootSignature = mComputeRootSignature.Get();
    compute.CS = { mComputeShader->GetBufferPointer(), mComputeShader->GetBufferSize() };
    ThrowIfFailed(device->CreateComputePipelineState(
        &compute, IID_PPV_ARGS(&mComputePSO)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC render = {};
    render.pRootSignature = mRenderRootSignature.Get();
    render.VS = { mVertexShader->GetBufferPointer(), mVertexShader->GetBufferSize() };
    render.GS = { mGeometryShader->GetBufferPointer(), mGeometryShader->GetBufferSize() };
    render.PS = { mPixelShader->GetBufferPointer(), mPixelShader->GetBufferSize() };
    D3D12_RASTERIZER_DESC rasterizer = CD3DX12_RASTERIZER_DESC_DEFAULT();
    rasterizer.CullMode = D3D12_CULL_MODE_NONE;
    render.RasterizerState = rasterizer;
    render.BlendState = CD3DX12_BLEND_DESC_DEFAULT();
    render.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
    render.SampleMask = UINT_MAX;
    render.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
    render.NumRenderTargets = kGBufferCount;
    for (UINT i = 0; i < kGBufferCount; ++i)
        render.RTVFormats[i] = kGBufferFormats[i];
    render.DSVFormat = depthFormat;
    render.SampleDesc.Count = 1;
    ThrowIfFailed(device->CreateGraphicsPipelineState(
        &render, IID_PPV_ARGS(&mRenderPSO)));
}

void ParticleSystem::Update(ID3D12GraphicsCommandList* commandList,
                            float deltaTime, float totalTime)
{
    const UINT outputBuffer = 1u - mActiveBuffer;
    if (mParticleStates[mActiveBuffer] != D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    {
        auto barrier = CD3DX12_RESOURCE_BARRIER_TRANSITION(
            mParticleBuffers[mActiveBuffer].Get(), mParticleStates[mActiveBuffer],
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commandList->ResourceBarrier(1, &barrier);
        mParticleStates[mActiveBuffer] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }

    auto counterToCopy = CD3DX12_RESOURCE_BARRIER_TRANSITION(
        mCounterBuffers[outputBuffer].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_COPY_DEST);
    commandList->ResourceBarrier(1, &counterToCopy);
    commandList->CopyBufferRegion(
        mCounterBuffers[outputBuffer].Get(), 0, mCounterUpload.Get(), 4, 4);
    auto counterToUav = CD3DX12_RESOURCE_BARRIER_TRANSITION(
        mCounterBuffers[outputBuffer].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    commandList->ResourceBarrier(1, &counterToUav);

    struct ComputeConstants
    {
        float DeltaTime;
        float TotalTime;
        UINT ParticleCount;
        float Pad0;
        XMFLOAT3 EmitterPosition;
        float Pad1;
    } constants = {
        (std::min)(deltaTime, 1.f / 20.f), totalTime, MaxParticles, 0.f,
        { -0.6f, -0.9f, 2.7f }, 0.f };

    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(mComputePSO.Get());
    commandList->SetComputeRootSignature(mComputeRootSignature.Get());
    auto heapStart = mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
    commandList->SetComputeRootDescriptorTable(0,
        CD3DX12_GPU_HANDLE(heapStart, mActiveBuffer, mDescriptorSize));
    commandList->SetComputeRootDescriptorTable(1,
        CD3DX12_GPU_HANDLE(heapStart, outputBuffer, mDescriptorSize));
    commandList->SetComputeRoot32BitConstants(2, 8, &constants, 0);
    commandList->Dispatch((MaxParticles + 255) / 256, 1, 1);

    D3D12_RESOURCE_BARRIER uavBarriers[2] = {};
    uavBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[0].UAV.pResource = mParticleBuffers[mActiveBuffer].Get();
    uavBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarriers[1].UAV.pResource = mParticleBuffers[outputBuffer].Get();
    commandList->ResourceBarrier(2, uavBarriers);

    mActiveBuffer = outputBuffer;
    auto toShaderResource = CD3DX12_RESOURCE_BARRIER_TRANSITION(
        mParticleBuffers[mActiveBuffer].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShaderResource);
    mParticleStates[mActiveBuffer] = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

void ParticleSystem::Render(ID3D12GraphicsCommandList* commandList,
                            FXMMATRIX viewProjection,
                            FXMVECTOR cameraRight,
                            FXMVECTOR cameraUp,
                            FXMVECTOR cameraForward)
{
    RenderConstants constants = {};
    XMStoreFloat4x4(&constants.ViewProjection, XMMatrixTranspose(viewProjection));
    XMStoreFloat3(&constants.CameraRight, cameraRight);
    XMStoreFloat3(&constants.CameraUp, cameraUp);
    XMStoreFloat3(&constants.CameraForward, cameraForward);
    constants.ParticleScale = 1.f;

    ID3D12DescriptorHeap* heaps[] = { mDescriptorHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetPipelineState(mRenderPSO.Get());
    commandList->SetGraphicsRootSignature(mRenderRootSignature.Get());
    commandList->SetGraphicsRootDescriptorTable(0,
        CD3DX12_GPU_HANDLE(mDescriptorHeap->GetGPUDescriptorHandleForHeapStart(),
            2 + mActiveBuffer, mDescriptorSize));
    commandList->SetGraphicsRoot32BitConstants(
        1, sizeof(RenderConstants) / 4, &constants, 0);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
    commandList->DrawInstanced(MaxParticles, 1, 0, 0);
}
