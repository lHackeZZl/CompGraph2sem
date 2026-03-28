#pragma once
// d3dApp.h — Base DirectX 12 application (self-contained)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <DirectXColors.h>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cassert>
#include <comdef.h>

#include "d3dx12.h"
#include "GameTimer.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

// ─── Exception helper ────────────────────────────────────────────────────────
class DxException
{
public:
    DxException(HRESULT hr, const std::wstring& fn, const std::wstring& file, int line)
        : ErrorCode(hr), FunctionName(fn), Filename(file), LineNumber(line) {}

    std::wstring ToString() const
    {
        _com_error err(ErrorCode);
        return FunctionName + L" failed in " + Filename +
               L"; line " + std::to_wstring(LineNumber) +
               L"; error: " + err.ErrorMessage();
    }
    HRESULT ErrorCode;
    std::wstring FunctionName, Filename;
    int LineNumber;
};

#ifndef ThrowIfFailed
#define ThrowIfFailed(x) {                                          \
    HRESULT hr__ = (x);                                            \
    if(FAILED(hr__)) throw DxException(hr__, L#x, __FILEW__, __LINE__); }
#endif

// ─── Geometry helpers ─────────────────────────────────────────────────────────
struct SubmeshGeometry
{
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    INT  BaseVertexLocation = 0;
};

struct MeshGeometry
{
    std::string Name;
    Microsoft::WRL::ComPtr<ID3DBlob>       VertexBufferCPU, IndexBufferCPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferGPU, IndexBufferGPU;
    Microsoft::WRL::ComPtr<ID3D12Resource> VertexBufferUploader, IndexBufferUploader;

    UINT VertexByteStride = 0;
    UINT VertexBufferByteSize = 0;
    DXGI_FORMAT IndexFormat = DXGI_FORMAT_R16_UINT;
    UINT IndexBufferByteSize = 0;
    std::unordered_map<std::string, SubmeshGeometry> DrawArgs;

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const
    {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = VertexBufferGPU->GetGPUVirtualAddress();
        vbv.SizeInBytes    = VertexBufferByteSize;
        vbv.StrideInBytes  = VertexByteStride;
        return vbv;
    }
    D3D12_INDEX_BUFFER_VIEW IndexBufferView() const
    {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = IndexBufferGPU->GetGPUVirtualAddress();
        ibv.SizeInBytes    = IndexBufferByteSize;
        ibv.Format         = IndexFormat;
        return ibv;
    }
};

// ─── d3dUtil ──────────────────────────────────────────────────────────────────
namespace d3dUtil
{
    inline UINT CalcConstantBufferByteSize(UINT s) { return (s + 255) & ~255; }

    Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(
        const std::wstring& filename, const D3D_SHADER_MACRO* defines,
        const std::string& entry, const std::string& target);

    Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(
        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
        const void* data, UINT64 byteSize,
        Microsoft::WRL::ComPtr<ID3D12Resource>& uploadBuffer);
}

// ─── D3DApp ──────────────────────────────────────────────────────────────────
class D3DApp
{
public:
    static D3DApp* GetApp() { return mApp; }
    explicit D3DApp(HINSTANCE h);
    D3DApp(const D3DApp&) = delete;
    D3DApp& operator=(const D3DApp&) = delete;
    virtual ~D3DApp();

    virtual bool Initialize();
    int  Run();
    virtual LRESULT MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    float AspectRatio() const;

protected:
    virtual void OnResize();
    virtual void Update(const GameTimer& gt) = 0;
    virtual void Draw  (const GameTimer& gt) = 0;
    virtual void OnMouseDown(WPARAM, int, int) {}
    virtual void OnMouseUp  (WPARAM, int, int) {}
    virtual void OnMouseMove(WPARAM, int, int) {}

    void FlushCommandQueue();
    void CalculateFrameStats();
    bool InitMainWindow();
    bool InitDirect3D();
    void CreateCommandObjects();
    void CreateSwapChain();
    void CreateRtvAndDsvDescriptorHeaps();

    ID3D12Resource*             CurrentBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrentBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;

    // ── Members ───────────────────────────────────────────────────────────────
    static D3DApp* mApp;

    HINSTANCE mhAppInst  = nullptr;
    HWND      mhMainWnd  = nullptr;
    bool      mAppPaused = false, mMinimized = false;
    bool      mMaximized = false, mResizing  = false;

    bool m4xMsaaState   = false;
    UINT m4xMsaaQuality = 0;

    GameTimer mTimer;

    Microsoft::WRL::ComPtr<IDXGIFactory4>   mdxgiFactory;
    Microsoft::WRL::ComPtr<IDXGISwapChain>  mSwapChain;
    Microsoft::WRL::ComPtr<ID3D12Device>    md3dDevice;
    Microsoft::WRL::ComPtr<ID3D12Fence>     mFence;
    UINT64 mCurrentFence = 0;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        mCommandQueue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    mDirectCmdListAlloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> mCommandList;

    static const int SwapChainBufferCount = 2;
    int mCurrBackBuffer = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource>  mSwapChainBuffer[SwapChainBufferCount];
    Microsoft::WRL::ComPtr<ID3D12Resource>  mDepthStencilBuffer;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;

    D3D12_VIEWPORT mScreenViewport{};
    D3D12_RECT     mScissorRect{};

    UINT mRtvDescriptorSize       = 0;
    UINT mDsvDescriptorSize       = 0;
    UINT mCbvSrvUavDescriptorSize = 0;

    std::wstring mMainWndCaption = L"DX12 Phong";
    int mClientWidth  = 800;
    int mClientHeight = 600;

    DXGI_FORMAT mBackBufferFormat   = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT mDepthStencilFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;

    DirectX::XMFLOAT4X4 mProj;
};
