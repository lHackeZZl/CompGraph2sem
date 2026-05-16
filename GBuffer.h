#pragma once
// GBuffer.h — инкапсулирует G-Buffer render targets и их дескрипторы
//
// Layout:
//   RT0  R16G16B16A16_FLOAT  — World Position (xyz) + unused
//   RT1  R16G16B16A16_FLOAT  — World Normal   (xyz) + unused
//   RT2  R8G8B8A8_UNORM      — Albedo (rgb) + Shininess/512 (a)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>
#include "d3dx12.h"

using Microsoft::WRL::ComPtr;

static const UINT        kGBufferCount      = 3;
static const DXGI_FORMAT kGBufferFormats[kGBufferCount] = {
    DXGI_FORMAT_R16G16B16A16_FLOAT,   // RT0: World Position
    DXGI_FORMAT_R16G16B16A16_FLOAT,   // RT1: World Normal
    DXGI_FORMAT_R8G8B8A8_UNORM,       // RT2: Albedo + Shininess
};

class GBuffer
{
public:
    GBuffer() = default;

    // Создаёт/пересоздаёт RT при изменении размера окна.
    // rtvHeap           — отдельная RTV-куча (kGBufferCount слотов)
    // srvHeap           — общая CBV/SRV/UAV куча приложения
    // srvBaseIndex      — индекс первого слота под G-Buffer SRV
    void Create(ID3D12Device*         device,
                UINT                  width,
                UINT                  height,
                ID3D12DescriptorHeap* rtvHeap,
                UINT                  rtvDescSize,
                ID3D12DescriptorHeap* srvHeap,
                UINT                  srvBaseIndex,
                UINT                  cbvSrvUavDescSize);

    void Release();

    // Geometry pass: переход RT → RENDER_TARGET, ClearRTV, OMSetRenderTargets
    void BeginGeometryPass(ID3D12GraphicsCommandList* cmd,
                           D3D12_CPU_DESCRIPTOR_HANDLE dsv,
                           UINT rtvDescSize);

    // Geometry pass end: переход RT → PIXEL_SHADER_RESOURCE
    void EndGeometryPass(ID3D12GraphicsCommandList* cmd);

    // GPU-хэндл первого SRV (три подряд — Position, Normal, Albedo)
    D3D12_GPU_DESCRIPTOR_HANDLE FirstSrvGpuHandle() const { return mFirstSrvGpu; }

    UINT Count() const { return kGBufferCount; }

private:
    ComPtr<ID3D12Resource>      mRTs[kGBufferCount];
    D3D12_CPU_DESCRIPTOR_HANDLE mRtvCpu[kGBufferCount] = {};
    D3D12_GPU_DESCRIPTOR_HANDLE mFirstSrvGpu           = {};
    UINT mWidth = 0, mHeight = 0;
};
