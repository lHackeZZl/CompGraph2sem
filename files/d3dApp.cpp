// d3dApp.cpp
#include "d3dApp.h"
#include <comdef.h>
#include <cassert>
using namespace DirectX;
using Microsoft::WRL::ComPtr;

D3DApp* D3DApp::mApp = nullptr;

// ── d3dUtil ──────────────────────────────────────────────────────────────────
ComPtr<ID3DBlob> d3dUtil::CompileShader(
    const std::wstring& filename, const D3D_SHADER_MACRO* defines,
    const std::string& entry, const std::string& target)
{
    UINT flags = 0;
#if defined(DEBUG)||defined(_DEBUG)
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> code, errors;
    HRESULT hr = D3DCompileFromFile(filename.c_str(), defines,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry.c_str(), target.c_str(), flags, 0, &code, &errors);
    if (errors) OutputDebugStringA((char*)errors->GetBufferPointer());
    ThrowIfFailed(hr);
    return code;
}

ComPtr<ID3D12Resource> d3dUtil::CreateDefaultBuffer(
    ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    const void* data, UINT64 byteSize, ComPtr<ID3D12Resource>& upload)
{
    ComPtr<ID3D12Resource> def;
    auto defHeap  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uplHeap  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto bufDesc  = CD3DX12_RESOURCE_DESC_BUFFER(byteSize);

    ThrowIfFailed(device->CreateCommittedResource(
        &defHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&def)));
    ThrowIfFailed(device->CreateCommittedResource(
        &uplHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    BYTE* p = nullptr;
    upload->Map(0, nullptr, (void**)&p);
    memcpy(p, data, byteSize);
    upload->Unmap(0, nullptr);

    auto b1 = CD3DX12_RESOURCE_BARRIER_TRANSITION(def.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmdList->ResourceBarrier(1, &b1);
    cmdList->CopyBufferRegion(def.Get(), 0, upload.Get(), 0, byteSize);
    auto b2 = CD3DX12_RESOURCE_BARRIER_TRANSITION(def.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmdList->ResourceBarrier(1, &b2);

    return def;
}

// ── Win32 ────────────────────────────────────────────────────────────────────
static LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{ return D3DApp::GetApp()->MsgProc(h,m,w,l); }

D3DApp::D3DApp(HINSTANCE h) : mhAppInst(h)
{
    assert(!mApp); mApp = this;
    XMStoreFloat4x4(&mProj, XMMatrixIdentity());
}
D3DApp::~D3DApp() { if (md3dDevice) FlushCommandQueue(); }
float D3DApp::AspectRatio() const { return (float)mClientWidth/(float)mClientHeight; }

bool D3DApp::Initialize()
{
    if (!InitMainWindow()) return false;
    if (!InitDirect3D())   return false;
    OnResize();
    return true;
}

bool D3DApp::InitMainWindow()
{
    WNDCLASS wc={};
    wc.style=CS_HREDRAW|CS_VREDRAW; wc.lpfnWndProc=MainWndProc;
    wc.hInstance=mhAppInst; wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    wc.hbrBackground=(HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName=L"MainWnd";
    if (!RegisterClass(&wc)) { MessageBox(nullptr,L"RegisterClass failed",nullptr,0); return false; }
    RECT r={0,0,mClientWidth,mClientHeight};
    AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,false);
    mhMainWnd=CreateWindow(L"MainWnd",mMainWndCaption.c_str(),WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,
        nullptr,nullptr,mhAppInst,nullptr);
    if (!mhMainWnd) { MessageBox(nullptr,L"CreateWindow failed",nullptr,0); return false; }
    ShowWindow(mhMainWnd,SW_SHOW); UpdateWindow(mhMainWnd);
    return true;
}

bool D3DApp::InitDirect3D()
{
#if defined(DEBUG)||defined(_DEBUG)
    { ComPtr<ID3D12Debug> dbg; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(); }
#endif
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory)));
    HRESULT hr = D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&md3dDevice));
    if(FAILED(hr)){
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ThrowIfFailed(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&md3dDevice)));
    }
    ThrowIfFailed(md3dDevice->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&mFence)));
    mRtvDescriptorSize       = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    mDsvDescriptorSize       = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS ms={};
    ms.Format=mBackBufferFormat; ms.SampleCount=4;
    ms.Flags=D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
    md3dDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,&ms,sizeof(ms));
    m4xMsaaQuality=ms.NumQualityLevels;

    CreateCommandObjects();
    CreateSwapChain();
    CreateRtvAndDsvDescriptorHeaps();
    return true;
}

void D3DApp::CreateCommandObjects()
{
    D3D12_COMMAND_QUEUE_DESC q={};
    q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT; q.Flags=D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(md3dDevice->CreateCommandQueue(&q,IID_PPV_ARGS(&mCommandQueue)));
    ThrowIfFailed(md3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&mDirectCmdListAlloc)));
    ThrowIfFailed(md3dDevice->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        mDirectCmdListAlloc.Get(),nullptr,IID_PPV_ARGS(&mCommandList)));
    mCommandList->Close();
}

void D3DApp::CreateSwapChain()
{
    mSwapChain=nullptr;
    DXGI_SWAP_CHAIN_DESC sd={};
    sd.BufferDesc.Width=mClientWidth; sd.BufferDesc.Height=mClientHeight;
    sd.BufferDesc.RefreshRate.Numerator=60; sd.BufferDesc.RefreshRate.Denominator=1;
    sd.BufferDesc.Format=mBackBufferFormat;
    sd.SampleDesc.Count=m4xMsaaState?4:1;
    sd.SampleDesc.Quality=m4xMsaaState?(m4xMsaaQuality-1):0;
    sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=SwapChainBufferCount;
    sd.OutputWindow=mhMainWnd; sd.Windowed=true;
    sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags=DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    ThrowIfFailed(mdxgiFactory->CreateSwapChain(mCommandQueue.Get(),&sd,&mSwapChain));
}

void D3DApp::CreateRtvAndDsvDescriptorHeaps()
{
    D3D12_DESCRIPTOR_HEAP_DESC rd={};
    rd.NumDescriptors=SwapChainBufferCount; rd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rd,IID_PPV_ARGS(&mRtvHeap)));
    D3D12_DESCRIPTOR_HEAP_DESC dd={};
    dd.NumDescriptors=1; dd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&dd,IID_PPV_ARGS(&mDsvHeap)));
}

void D3DApp::OnResize()
{
    assert(md3dDevice&&mSwapChain&&mDirectCmdListAlloc);
    FlushCommandQueue();
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(),nullptr));

    for(int i=0;i<SwapChainBufferCount;++i) mSwapChainBuffer[i]=nullptr;
    mDepthStencilBuffer=nullptr;

    ThrowIfFailed(mSwapChain->ResizeBuffers(SwapChainBufferCount,
        mClientWidth,mClientHeight,mBackBufferFormat,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
    mCurrBackBuffer=0;

    // RTVs
    D3D12_CPU_DESCRIPTOR_HANDLE rtvH = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for(UINT i=0;i<SwapChainBufferCount;++i){
        ThrowIfFailed(mSwapChain->GetBuffer(i,IID_PPV_ARGS(&mSwapChainBuffer[i])));
        md3dDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(),nullptr,rtvH);
        rtvH = CD3DX12_CPU_HANDLE(rtvH,1,mRtvDescriptorSize);
    }

    // DSV
    auto defHeap  = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto depthDesc= CD3DX12_RESOURCE_DESC_TEX2D(mDepthStencilFormat,
        mClientWidth,mClientHeight,1,1,
        m4xMsaaState?4:1, m4xMsaaState?(m4xMsaaQuality-1):0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    auto optClear = CD3DX12_CLEAR_VALUE_DEPTH(mDepthStencilFormat,1.f,0);

    ThrowIfFailed(md3dDevice->CreateCommittedResource(
        &defHeap,D3D12_HEAP_FLAG_NONE,&depthDesc,
        D3D12_RESOURCE_STATE_COMMON,&optClear,
        IID_PPV_ARGS(&mDepthStencilBuffer)));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc={};
    dsvDesc.Flags=D3D12_DSV_FLAG_NONE;
    dsvDesc.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Format=mDepthStencilFormat;
    md3dDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(),&dsvDesc,DepthStencilView());

    auto b = CD3DX12_RESOURCE_BARRIER_TRANSITION(mDepthStencilBuffer.Get(),
        D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    mCommandList->ResourceBarrier(1,&b);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[]={mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1,c);
    FlushCommandQueue();

    mScreenViewport={0,0,(float)mClientWidth,(float)mClientHeight,0,1};
    mScissorRect={0,0,mClientWidth,mClientHeight};

    XMMATRIX P=XMMatrixPerspectiveFovLH(0.25f*XM_PI,AspectRatio(),1.f,1000.f);
    XMStoreFloat4x4(&mProj,P);
}

void D3DApp::FlushCommandQueue()
{
    ++mCurrentFence;
    ThrowIfFailed(mCommandQueue->Signal(mFence.Get(),mCurrentFence));
    if(mFence->GetCompletedValue()<mCurrentFence){
        HANDLE ev=CreateEventEx(nullptr,nullptr,0,EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence,ev));
        WaitForSingleObject(ev,INFINITE); CloseHandle(ev);
    }
}

ID3D12Resource* D3DApp::CurrentBackBuffer() const
{ return mSwapChainBuffer[mCurrBackBuffer].Get(); }

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrentBackBufferView() const
{ return CD3DX12_CPU_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart(),mCurrBackBuffer,mRtvDescriptorSize); }

D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::DepthStencilView() const
{ return mDsvHeap->GetCPUDescriptorHandleForHeapStart(); }

int D3DApp::Run()
{
    MSG msg={};
    mTimer.Reset();
    while(msg.message!=WM_QUIT){
        if(PeekMessage(&msg,nullptr,0,0,PM_REMOVE)){
            TranslateMessage(&msg); DispatchMessage(&msg);
        } else {
            mTimer.Tick();
            if(!mAppPaused){ CalculateFrameStats(); Update(mTimer); Draw(mTimer); }
            else Sleep(100);
        }
    }
    return (int)msg.wParam;
}

void D3DApp::CalculateFrameStats()
{
    static int fc=0; static float te=0.f; ++fc;
    if(mTimer.TotalTime()-te>=1.f){
        float fps=(float)fc, ms=1000.f/fps;
        std::wstring t=mMainWndCaption+L"  fps:"+std::to_wstring((int)fps)+L"  ms:"+std::to_wstring(ms);
        SetWindowText(mhMainWnd,t.c_str());
        fc=0; te+=1.f;
    }
}

LRESULT D3DApp::MsgProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    switch(msg){
    case WM_ACTIVATE:
        if(LOWORD(w)==WA_INACTIVE){mAppPaused=true;mTimer.Stop();}
        else{mAppPaused=false;mTimer.Start();}
        return 0;
    case WM_SIZE:
        mClientWidth=LOWORD(l); mClientHeight=HIWORD(l);
        if(md3dDevice){
            if(w==SIZE_MINIMIZED){mAppPaused=true;mMinimized=true;mMaximized=false;}
            else if(w==SIZE_MAXIMIZED){mAppPaused=false;mMinimized=false;mMaximized=true;OnResize();}
            else if(w==SIZE_RESTORED){
                if(mMinimized){mAppPaused=false;mMinimized=false;OnResize();}
                else if(mMaximized){mAppPaused=false;mMaximized=false;OnResize();}
                else if(!mResizing){OnResize();}
            }
        }
        return 0;
    case WM_ENTERSIZEMOVE: mAppPaused=true;mResizing=true;mTimer.Stop(); return 0;
    case WM_EXITSIZEMOVE:  mAppPaused=false;mResizing=false;mTimer.Start();OnResize(); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_MENUCHAR: return MAKELRESULT(0,MNC_CLOSE);
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize={200,200}; return 0;
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
        OnMouseDown(w,GET_X_LPARAM(l),GET_Y_LPARAM(l)); return 0;
    case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
        OnMouseUp(w,GET_X_LPARAM(l),GET_Y_LPARAM(l)); return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(w,GET_X_LPARAM(l),GET_Y_LPARAM(l)); return 0;
    case WM_KEYUP:
        if(w==VK_ESCAPE) DestroyWindow(mhMainWnd);
        return 0;
    }
    return DefWindowProc(hwnd,msg,w,l);
}
