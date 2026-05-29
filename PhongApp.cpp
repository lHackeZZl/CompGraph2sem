// PhongApp.cpp — Deferred Rendering через RenderingSystem + GBuffer
#include "PhongApp.h"
#include <fstream>
#include <sstream>
#include <comdef.h>
#include <wincodec.h>
#pragma comment(lib,"windowscodecs.lib")

// ─── FrameResource ────────────────────────────────────────────────────────────
FrameResource::FrameResource(ID3D12Device* d, UINT pass, UINT obj)
{
    ThrowIfFailed(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&CmdListAlloc)));
    PassCB   = std::make_unique<UploadBuffer<CBPerPass  >>(d, pass, true);
    ObjectCB = std::make_unique<UploadBuffer<CBPerObject>>(d, obj,  true);
}

// ─── PhongApp ─────────────────────────────────────────────────────────────────
PhongApp::PhongApp(HINSTANCE h) : D3DApp(h)
{ mMainWndCaption = L"DX12 — Breakfast Room [Deferred Rendering]"; }

PhongApp::~PhongApp() { if (md3dDevice) FlushCommandQueue(); }

bool PhongApp::Initialize()
{
    if (!D3DApp::Initialize()) return false;
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    SetupLights();

    // Инициализируем RenderingSystem — он строит RS, PSO, quad, lighting CBs
    mRenderer.Initialize(
        md3dDevice.Get(), mCommandList.Get(),
        mBackBufferFormat, mDepthStencilFormat,
        mCbvSrvUavDescriptorSize, mRtvDescriptorSize);

    // Грузим сцену
    bool loaded = LoadScene("breakfast_room.obj");
    if (!loaded) loaded = LoadScene("model.obj");
    if (!loaded) BuildDefaultCube();

    // Вертексная анимация для картин/светильников
    static const std::vector<std::string> kAnimMats = {
        "breakfast_room:Artwork","breakfast_room:Chrome",
        "breakfast_room:Frosted_Glass","breakfast_room:Gold_Paint",
    };
    for (auto& ri : mAllRItems)
        for (const auto& am : kAnimMats)
            if (ri->SubMesh == am) { ri->AnimEnabled = true; ri->NumFramesDirty = 3; break; }

    if (mTextures.empty()) CreateProceduralTexture(mFallbackTexName);

    // Создаём видимые объекты-источники света заранее: маленькие сферы,
    // которыми потом можно стрелять из камеры. Они входят в ObjectCB layout.
    BuildLightSphereObjects();

    BuildFrameResources();
    BuildDescriptorHeaps();         // создаёт mCbvSrvHeap и mGBufferRtvHeap
    BuildConstantBufferViews();     // obj CBVs + pass CBVs

    // Lighting CBVs строит RenderingSystem через наш heap
    mRenderer.BuildLightingViews(
        md3dDevice.Get(), mCbvSrvHeap.Get(),
        mLightCbvOffset, mPointLightSrvOffset, mCbvSrvUavDescriptorSize);

    BuildTextureViews();

    // G-Buffer: создаём RT и SRV (в mCbvSrvHeap начиная с mGBufSrvOffset)
    mRenderer.GetGBuffer().Create(
        md3dDevice.Get(),
        (UINT)mClientWidth, (UINT)mClientHeight,
        mGBufferRtvHeap.Get(), mRtvDescriptorSize,
        mCbvSrvHeap.Get(), mGBufSrvOffset,
        mCbvSrvUavDescriptorSize);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, c);
    FlushCommandQueue();
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Lights  (1 Directional + 3 Point + 1 Spot)
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::SetupLights()
{
    // Реальные границы сцены breakfast_room:
    // X: [-6.6 .. +5.4]  центр X = -0.6
    // Y: [-1.74 .. 8.11] пол=-1.74, потолок=8.11
    // Z: [-4.77 .. 10.16] центр Z = 2.7

    mLightObjects.clear();
    mPointLights.clear();

    // Directional — слабый нейтральный
    mLightingData.DirLight.Direction = { 0.3f, -1.0f, 0.3f };
    mLightingData.DirLight.Ambient   = { 0.20f, 0.20f, 0.20f, 1.f };
    mLightingData.DirLight.Diffuse   = { 0.25f, 0.25f, 0.25f, 1.f };
    mLightingData.DirLight.Specular  = { 0.2f,  0.2f,  0.2f,  1.f };

    auto addStaticPointLight = [&](const PointLight& L)
    {
        LightObject obj{};
        obj.Light  = L;
        obj.Active = true;
        obj.Moving = false;
        mLightObjects.push_back(obj);
        mPointLights.push_back(L);
    };

    PointLight L{};

    // Point 0 — оранжево-красный, левая часть потолка
    L.Position    = { -3.0f, 7.5f,  2.7f };
    L.Range       = 200.f;
    L.Ambient     = { 0.18f, 0.04f, 0.00f, 1.f };
    L.Diffuse     = { 1.0f,  0.25f, 0.00f, 1.f };
    L.Specular    = { 1.0f,  0.30f, 0.00f, 1.f };
    L.Attenuation = { 1.f, 0.007f, 0.0002f };
    addStaticPointLight(L);

    // Point 1 — синий, правая часть потолка
    L.Position    = {  2.0f, 7.5f,  2.7f };
    L.Range       = 200.f;
    L.Ambient     = { 0.00f, 0.03f, 0.18f, 1.f };
    L.Diffuse     = { 0.00f, 0.30f, 1.0f,  1.f };
    L.Specular    = { 0.00f, 0.35f, 1.0f,  1.f };
    L.Attenuation = { 1.f, 0.007f, 0.0002f };
    addStaticPointLight(L);

    // Point 2 — зелёный, центр потолка
    L.Position    = { -0.6f, 7.8f,  2.7f };
    L.Range       = 200.f;
    L.Ambient     = { 0.00f, 0.12f, 0.00f, 1.f };
    L.Diffuse     = { 0.00f, 1.00f, 0.10f, 1.f };
    L.Specular    = { 0.00f, 1.00f, 0.15f, 1.f };
    L.Attenuation = { 1.f, 0.005f, 0.0001f };
    addStaticPointLight(L);

    // Spot — пурпурный, направлен вниз над центром стола
    mLightingData.Spot.Position    = { -0.6f, 7.5f,  2.7f };
    mLightingData.Spot.Range       = 200.f;
    mLightingData.Spot.Direction   = {  0.0f,-1.0f,  0.0f };
    mLightingData.Spot.SpotPower   = 4.f;   // широкий конус — вся комната
    mLightingData.Spot.Ambient     = { 0.08f, 0.00f, 0.08f, 1.f };
    mLightingData.Spot.Diffuse     = { 1.00f, 0.00f, 1.00f, 1.f };
    mLightingData.Spot.Specular    = { 1.00f, 0.10f, 1.00f, 1.f };
    mLightingData.Spot.Attenuation = { 1.f, 0.007f, 0.0002f };

    mLightingData.NumPointLights = (int)mPointLights.size();
    mLightingData.HasSpot        = 1;
}

// ════════════════════════════════════════════════════════════════════════════
// Descriptor Heaps
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildDescriptorHeaps()
{
    UINT n    = (UINT)mAllRItems.size();
    UINT texN = (UINT)mTextures.size();

    mPassCbvOffset  = n * 3;
    mLightCbvOffset = mPassCbvOffset + 3;
    mPointLightSrvOffset = mLightCbvOffset + 3;
    mSrvBaseOffset  = mPointLightSrvOffset + 3;
    mGBufSrvOffset  = mSrvBaseOffset + texN;

    UINT total = mGBufSrvOffset + kGBufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.NumDescriptors = total;
    d.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&d, IID_PPV_ARGS(&mCbvSrvHeap)));

    // Отдельная RTV-куча для G-Buffer (kGBufferCount слотов)
    D3D12_DESCRIPTOR_HEAP_DESC rd = {};
    rd.NumDescriptors = kGBufferCount;
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&mGBufferRtvHeap)));
}

void PhongApp::BuildConstantBufferViews()
{
    UINT objSz  = d3dUtil::CalcConstantBufferByteSize(sizeof(CBPerObject));
    UINT passSz = d3dUtil::CalcConstantBufferByteSize(sizeof(CBPerPass));
    UINT n      = (UINT)mAllRItems.size();

    // Object CBVs
    for (int fr = 0; fr < 3; ++fr) {
        auto cb = mFrameResources[fr]->ObjectCB->Resource();
        for (UINT i = 0; i < n; ++i) {
            auto h = CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                fr*n + i, mCbvSrvUavDescriptorSize);
            D3D12_CONSTANT_BUFFER_VIEW_DESC v = {
                cb->GetGPUVirtualAddress() + (UINT64)i*objSz, objSz };
            md3dDevice->CreateConstantBufferView(&v, h);
        }
    }
    // Pass CBVs
    for (int fr = 0; fr < 3; ++fr) {
        auto h = CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
            mPassCbvOffset + fr, mCbvSrvUavDescriptorSize);
        auto cb = mFrameResources[fr]->PassCB->Resource();
        D3D12_CONSTANT_BUFFER_VIEW_DESC v = { cb->GetGPUVirtualAddress(), passSz };
        md3dDevice->CreateConstantBufferView(&v, h);
    }
}

void PhongApp::BuildTextureViews()
{
    UINT idx = 0;
    for (auto& [name, tex] : mTextures) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format                  = tex->GetDesc().Format;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels     = 1;
        auto h = CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
            mSrvBaseOffset + idx, mCbvSrvUavDescriptorSize);
        md3dDevice->CreateShaderResourceView(tex.Get(), &sd, h);
        mTextureSrvIndex[name] = idx;
        ++idx;
    }
}

void PhongApp::BuildFrameResources()
{
    for (int i = 0; i < 3; ++i)
        mFrameResources.push_back(std::make_unique<FrameResource>(
            md3dDevice.Get(), 1, (UINT)mAllRItems.size()));
}

// ════════════════════════════════════════════════════════════════════════════
// OnResize — пересоздаём G-Buffer под новый размер
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::OnResize()
{
    D3DApp::OnResize();

    if (md3dDevice && mCbvSrvHeap)
    {
        FlushCommandQueue();
        mRenderer.GetGBuffer().Release();
        mRenderer.GetGBuffer().Create(
            md3dDevice.Get(),
            (UINT)mClientWidth, (UINT)mClientHeight,
            mGBufferRtvHeap.Get(), mRtvDescriptorSize,
            mCbvSrvHeap.Get(), mGBufSrvOffset,
            mCbvSrvUavDescriptorSize);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::Update(const GameTimer& gt)
{
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % 3;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();
    if (mCurrFrameResource->Fence != 0 &&
        mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE ev = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, ev));
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
    mTime += gt.DeltaTime();
    UpdateCamera(gt);
    UpdateLightObjects(gt);
    UpdateObjectCBs(gt);
    UpdatePassCB(gt);

    // Обновляем lighting CB через RenderingSystem
    mRenderer.UpdateLightingData(mCurrFrameResourceIndex, mLightingData, mPointLights);
}

void PhongApp::UpdateObjectCBs(const GameTimer&)
{
    auto* cb = mCurrFrameResource->ObjectCB.get();
    for (auto& e : mAllRItems) {
        if (e->NumFramesDirty <= 0) continue;
        XMMATRIX w   = XMLoadFloat4x4(&e->World);
        XMMATRIX wit = XMMatrixTranspose(XMMatrixInverse(nullptr, w));
        CBPerObject o{};
        XMStoreFloat4x4(&o.World,             XMMatrixTranspose(w));
        XMStoreFloat4x4(&o.WorldInvTranspose, XMMatrixTranspose(wit));
        o.MatAmbient  = e->Mat.Ambient;
        o.MatDiffuse  = e->Mat.Diffuse;
        o.MatSpecular = e->Mat.Specular;
        o.AnimEnabled = e->AnimEnabled ? 1.f : 0.f;
        cb->CopyData(e->ObjCBIndex, o);
        --e->NumFramesDirty;
    }
}

void PhongApp::UpdatePassCB(const GameTimer&)
{
    XMVECTOR eye, forward, right, up;
    GetCameraBasis(eye, forward, right, up);

    XMMATRIX view = XMMatrixLookToLH(eye, forward, up);
    XMMATRIX proj = XMLoadFloat4x4(&mProj);

    CBPerPass p{};
    XMStoreFloat4x4(&p.View,     XMMatrixTranspose(view));
    XMStoreFloat4x4(&p.Proj,     XMMatrixTranspose(proj));
    XMStoreFloat4x4(&p.ViewProj, XMMatrixTranspose(view * proj));
    XMStoreFloat3  (&p.EyePosW,  eye);
    p.TexScale  = {1.f, 1.f};
    p.TexOffset = {0.f, 0.f};
    p.Time      = mTime;
    mCurrFrameResource->PassCB->CopyData(0, p);

    XMStoreFloat3(&mLightingData.EyePosW, eye);
    mLightingData.NumPointLights = (int)mPointLights.size();
}

// ════════════════════════════════════════════════════════════════════════════
// Draw  —  два прохода через RenderingSystem
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::Draw(const GameTimer&)
{
    auto alloc = mCurrFrameResource->CmdListAlloc;
    ThrowIfFailed(alloc->Reset());
    ThrowIfFailed(mCommandList->Reset(alloc.Get(), nullptr));

    ID3D12DescriptorHeap* heaps[] = { mCbvSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, heaps);

    // ── Pass 1: Geometry → G-Buffer ───────────────────────────────────────────
    mRenderer.BeginGeometryPass(
        mCommandList.Get(), DepthStencilView(),
        mScreenViewport, mScissorRect);

    // Биндим pass CBV (slot 1 в GeometryRS)
    auto passH = CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mPassCbvOffset + mCurrFrameResourceIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, passH);

    DrawRenderItems(mCommandList.Get());

    mRenderer.EndGeometryPass(mCommandList.Get());

    // ── Pass 2: Lighting → Back Buffer ────────────────────────────────────────
    auto b1 = CD3DX12_RESOURCE_BARRIER_TRANSITION(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &b1);

    // GPU-хэндл lighting CBV текущего фрейма
    auto lightH = CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mLightCbvOffset + mCurrFrameResourceIndex, mCbvSrvUavDescriptorSize);

    auto pointLightsH = mRenderer.PointLightsSrvGpuHandle(
        mCbvSrvHeap.Get(), mCurrFrameResourceIndex, mCbvSrvUavDescriptorSize);

    mRenderer.LightingPass(
        mCommandList.Get(),
        CurrentBackBufferView(),
        lightH, pointLightsH,
        mScreenViewport, mScissorRect);

    auto b2 = CD3DX12_RESOURCE_BARRIER_TRANSITION(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &b2);

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, c);
    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer + 1) % 2;
    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

// ════════════════════════════════════════════════════════════════════════════
// DrawRenderItems — вызывается внутри geometry pass
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::DrawRenderItems(ID3D12GraphicsCommandList* cmd)
{
    UINT n = (UINT)mAllRItems.size();
    for (auto* ri : mOpaqueRItems)
    {
        if (!ri->Visible) continue;

        // Per-object CBV → slot 0
        auto objH = CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            mCurrFrameResourceIndex * n + ri->ObjCBIndex, mCbvSrvUavDescriptorSize);
        cmd->SetGraphicsRootDescriptorTable(0, objH);

        // Texture SRV → slot 2
        std::string texName = ri->TextureName;
        if (!mTextureSrvIndex.count(texName)) texName = mFallbackTexName;
        if (!mTextureSrvIndex.count(texName) && !mTextureSrvIndex.empty())
            texName = mTextureSrvIndex.begin()->first;
        UINT srvIdx = mTextureSrvIndex.count(texName) ? mTextureSrvIndex.at(texName) : 0;
        auto srvH = CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            mSrvBaseOffset + srvIdx, mCbvSrvUavDescriptorSize);
        cmd->SetGraphicsRootDescriptorTable(2, srvH);

        auto vbv = ri->Geo->VertexBufferView();
        auto ibv = ri->Geo->IndexBufferView();
        cmd->IASetVertexBuffers(0, 1, &vbv);
        cmd->IASetIndexBuffer(&ibv);
        cmd->IASetPrimitiveTopology(ri->PrimitiveType);
        auto& sub = ri->Geo->DrawArgs[ri->SubMesh];
        cmd->DrawIndexedInstanced(sub.IndexCount, 1,
            sub.StartIndexLocation, sub.BaseVertexLocation, 0);
    }
}

// ════════════════════════════════════════════════════════════════════════════
// Texture loading
// ════════════════════════════════════════════════════════════════════════════
bool PhongApp::LoadTexture(const std::string& name, const std::wstring& path)
{
    if (mTextures.count(name)) return true;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;

    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr,
        GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(dec->GetFrame(0, &frame))) return false;

    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom);

    UINT W, H; conv->GetSize(&W, &H);
    std::vector<BYTE> pixels(W * H * 4);
    conv->CopyPixels(nullptr, W*4, (UINT)pixels.size(), pixels.data());

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex, upload;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE,
        &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));

    UINT64 uploadSize = 0;
    md3dDevice->GetCopyableFootprints(&td,0,1,0,nullptr,nullptr,nullptr,&uploadSize);
    auto uplHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uplDesc = CD3DX12_RESOURCE_DESC_BUFFER(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uplHeap, D3D12_HEAP_FLAG_NONE,
        &uplDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSize;
    md3dDevice->GetCopyableFootprints(&td,0,1,0,&layout,&numRows,&rowSize,nullptr);

    BYTE* mapped = nullptr;
    upload->Map(0, nullptr, (void**)&mapped);
    for (UINT r = 0; r < numRows; ++r)
        memcpy(mapped + layout.Offset + r*layout.Footprint.RowPitch,
               pixels.data() + r*W*4, rowSize);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint = layout;
    mCommandList->CopyTextureRegion(&dst, 0,0,0, &src, nullptr);

    auto b = CD3DX12_RESOURCE_BARRIER_TRANSITION(tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &b);

    mTextures[name]       = tex;
    mTextureUploads[name] = upload;
    return true;
}

void PhongApp::CreateProceduralTexture(const std::string& name)
{
    const UINT W=256, H=256;
    std::vector<UINT> px(W*H);
    for (UINT y=0;y<H;++y)
        for (UINT x=0;x<W;++x)
            px[y*W+x] = ((x/32+y/32)%2==0) ? 0xFF4488FF : 0xFF112266;

    D3D12_RESOURCE_DESC td={};
    td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width=W; td.Height=H; td.DepthOrArraySize=1; td.MipLevels=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
    td.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex,upload;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&defHeap,D3D12_HEAP_FLAG_NONE,
        &td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex)));

    UINT64 uploadSize=0;
    md3dDevice->GetCopyableFootprints(&td,0,1,0,nullptr,nullptr,nullptr,&uploadSize);
    auto uplHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uplDesc=CD3DX12_RESOURCE_DESC_BUFFER(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uplHeap,D3D12_HEAP_FLAG_NONE,
        &uplDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSize;
    md3dDevice->GetCopyableFootprints(&td,0,1,0,&layout,&numRows,&rowSize,nullptr);

    BYTE* mapped=nullptr;
    upload->Map(0,nullptr,(void**)&mapped);
    for (UINT r=0;r<numRows;++r)
        memcpy(mapped+layout.Offset+r*layout.Footprint.RowPitch,
               (BYTE*)px.data()+r*W*4, rowSize);
    upload->Unmap(0,nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst={tex.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src={upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint=layout;
    mCommandList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
    auto b=CD3DX12_RESOURCE_BARRIER_TRANSITION(tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1,&b);
    mTextures[name]=tex; mTextureUploads[name]=upload;
}

void PhongApp::CreateSolidTexture(const std::string& name, UINT rgba)
{
    if (mTextures.count(name)) return;

    const UINT W = 4, H = 4;
    std::vector<UINT> px(W * H, rgba);

    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = W;
    td.Height = H;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> tex, upload;
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE,
        &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));

    UINT64 uploadSize = 0;
    md3dDevice->GetCopyableFootprints(&td, 0, 1, 0, nullptr, nullptr, nullptr, &uploadSize);
    auto uplHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uplDesc = CD3DX12_RESOURCE_DESC_BUFFER(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uplHeap, D3D12_HEAP_FLAG_NONE,
        &uplDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSize;
    md3dDevice->GetCopyableFootprints(&td, 0, 1, 0, &layout, &numRows, &rowSize, nullptr);

    BYTE* mapped = nullptr;
    upload->Map(0, nullptr, (void**)&mapped);
    for (UINT r = 0; r < numRows; ++r)
        memcpy(mapped + layout.Offset + r * layout.Footprint.RowPitch,
               (BYTE*)px.data() + r * W * 4, rowSize);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = { tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    D3D12_TEXTURE_COPY_LOCATION src = { upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint = layout;
    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    auto b = CD3DX12_RESOURCE_BARRIER_TRANSITION(tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &b);

    mTextures[name] = tex;
    mTextureUploads[name] = upload;
}

// ════════════════════════════════════════════════════════════════════════════
// MTL loader
// ════════════════════════════════════════════════════════════════════════════
std::unordered_map<std::string,ObjMaterial>
PhongApp::LoadMtl(const std::string& path)
{
    std::unordered_map<std::string,ObjMaterial> mats;
    std::ifstream f(path); if (!f) return mats;
    ObjMaterial* cur=nullptr;
    std::string line;
    while (std::getline(f,line)) {
        std::istringstream ss(line); std::string tok; ss>>tok;
        if      (tok=="newmtl"){ std::string n; ss>>n; mats[n]={}; mats[n].Name=n; cur=&mats[n]; }
        else if (cur) {
            if      (tok=="Ka"){ ss>>cur->Ambient.x>>cur->Ambient.y>>cur->Ambient.z; }
            else if (tok=="Kd"){ ss>>cur->Diffuse.x>>cur->Diffuse.y>>cur->Diffuse.z; }
            else if (tok=="Ks"){ ss>>cur->Specular.x>>cur->Specular.y>>cur->Specular.z; }
            else if (tok=="Ns"){ ss>>cur->Specular.w; }
            else if (tok=="map_Kd"){
                ss>>cur->DiffuseMap;
                // Если Kd=(0,0,0) но есть текстура — ставим белый,
                // иначе текстура умножается на чёрный → пол чёрный
                float kd = cur->Diffuse.x + cur->Diffuse.y + cur->Diffuse.z;
                if (kd < 0.01f)
                    cur->Diffuse = {1.f, 1.f, 1.f, 1.f};
            }
        }
    }
    return mats;
}

// ════════════════════════════════════════════════════════════════════════════
// OBJ loader
// ════════════════════════════════════════════════════════════════════════════
bool PhongApp::LoadScene(const std::string& objFile)
{
    std::ifstream f(objFile); if (!f) return false;

    std::string dir;
    auto sl=objFile.rfind('/');
    if (sl==std::string::npos) sl=objFile.rfind('\\');
    if (sl!=std::string::npos) dir=objFile.substr(0,sl+1);

    std::vector<XMFLOAT3> pos,nrm;
    std::vector<XMFLOAT2> uvs;

    struct MatGroup { ObjMaterial mat; std::vector<Vertex> verts; std::vector<uint32_t> idx; };
    std::unordered_map<std::string,MatGroup> groups;
    std::unordered_map<std::string,ObjMaterial> mats;
    std::string curMat="__default__"; groups[curMat]={};

    std::string line;
    while (std::getline(f,line)) {
        std::istringstream ss(line); std::string tok; ss>>tok;
        if      (tok=="v") { XMFLOAT3 p; ss>>p.x>>p.y>>p.z; pos.push_back(p); }
        else if (tok=="vn"){ XMFLOAT3 n; ss>>n.x>>n.y>>n.z; nrm.push_back(n); }
        else if (tok=="vt"){ XMFLOAT2 u; ss>>u.x>>u.y; u.y=1.f-u.y; uvs.push_back(u); }
        else if (tok=="mtllib"){ std::string mf; ss>>mf; mats=LoadMtl(dir+mf); }
        else if (tok=="usemtl"){
            ss>>curMat;
            if (!groups.count(curMat)){ groups[curMat]={}; if(mats.count(curMat)) groups[curMat].mat=mats[curMat]; }
        }
        else if (tok=="f"){
            std::vector<std::tuple<int,int,int>> face;
            std::string fc;
            while (ss>>fc){
                int vi=0,ti=-1,ni=-1;
                auto s1=fc.find('/'); vi=std::stoi(fc.substr(0,s1))-1;
                if (s1!=std::string::npos){
                    auto s2=fc.find('/',s1+1);
                    if (s2!=s1+1&&s2!=std::string::npos) ti=std::stoi(fc.substr(s1+1,s2-s1-1))-1;
                    else if (s1+1<fc.size()&&fc[s1+1]!='/') ti=std::stoi(fc.substr(s1+1))-1;
                    if (s2!=std::string::npos&&s2+1<fc.size()) ni=std::stoi(fc.substr(s2+1))-1;
                }
                face.push_back({vi,ti,ni});
            }
            auto& g=groups[curMat];
            for (size_t i=1;i+1<face.size();++i)
                for (auto fv:{face[0],face[i],face[i+1]}){
                    Vertex vtx{};
                    int vi=std::get<0>(fv),ti=std::get<1>(fv),ni=std::get<2>(fv);
                    if(vi>=0&&vi<(int)pos.size()) vtx.Pos=pos[vi];
                    if(ni>=0&&ni<(int)nrm.size()) vtx.Normal=nrm[ni];
                    if(ti>=0&&ti<(int)uvs.size()) vtx.TexCoord=uvs[ti];
                    g.idx.push_back((uint32_t)g.verts.size());
                    g.verts.push_back(vtx);
                }
        }
    }
    if (groups.empty()) return false;

    auto geo=std::make_unique<MeshGeometry>(); geo->Name="sceneGeo";
    std::vector<Vertex> allVerts; std::vector<uint32_t> allIdx;

    for (auto& [matName,g]:groups){
        if (g.verts.empty()) continue;
        SubmeshGeometry sub;
        sub.IndexCount=(UINT)g.idx.size();
        sub.StartIndexLocation=(UINT)allIdx.size();
        sub.BaseVertexLocation=(INT)allVerts.size();
        geo->DrawArgs[matName]=sub;
        for (auto& v:g.verts) allVerts.push_back(v);
        for (auto  i:g.idx)   allIdx.push_back(i);

        std::string texName=mFallbackTexName;
        if (!g.mat.DiffuseMap.empty()){
            texName=g.mat.DiffuseMap;
            if (!mTextures.count(texName)){
                std::wstring wp(dir.begin(),dir.end());
                std::wstring wn(g.mat.DiffuseMap.begin(),g.mat.DiffuseMap.end());
                if (!LoadTexture(texName,wp+wn)) texName=mFallbackTexName;
            }
        }
        auto ri=std::make_unique<RenderItem>();
        XMStoreFloat4x4(&ri->World,XMMatrixIdentity());
        ri->ObjCBIndex=( UINT)mAllRItems.size();
        ri->Geo=geo.get(); ri->SubMesh=matName;
        ri->TextureName=texName; ri->Mat=g.mat;
        mOpaqueRItems.push_back(ri.get());
        mAllRItems.push_back(std::move(ri));
    }
    if (allVerts.empty()) return false;

    UINT vbSz=(UINT)(allVerts.size()*sizeof(Vertex));
    UINT ibSz=(UINT)(allIdx.size()  *sizeof(uint32_t));
    ThrowIfFailed(D3DCreateBlob(vbSz,&geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(),allVerts.data(),vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz,&geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(),allIdx.data(),ibSz);
    geo->VertexBufferGPU=d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(),allVerts.data(),vbSz,geo->VertexBufferUploader);
    geo->IndexBufferGPU=d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(),allIdx.data(),ibSz,geo->IndexBufferUploader);
    geo->VertexByteStride=sizeof(Vertex);
    geo->VertexBufferByteSize=vbSz;
    geo->IndexFormat=DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize=ibSz;
    mGeometries["sceneGeo"]=std::move(geo);
    return true;
}

void PhongApp::BuildDefaultCube()
{
    GeometryGenerator g;
    auto cube=g.CreateBox(1.5f,1.5f,1.5f,0);
    std::vector<Vertex> verts(cube.Vertices.size());
    for (size_t i=0;i<cube.Vertices.size();++i){
        verts[i].Pos=cube.Vertices[i].Position;
        verts[i].Normal=cube.Vertices[i].Normal;
        verts[i].TexCoord=cube.Vertices[i].TexCoord;
    }
    auto idx16=cube.GetIndices16();
    std::vector<uint32_t> idx(idx16.begin(),idx16.end());
    UINT vbSz=(UINT)(verts.size()*sizeof(Vertex));
    UINT ibSz=(UINT)(idx.size()  *sizeof(uint32_t));

    auto geo=std::make_unique<MeshGeometry>(); geo->Name="cubeGeo";
    ThrowIfFailed(D3DCreateBlob(vbSz,&geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(),verts.data(),vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz,&geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(),idx.data(),ibSz);
    geo->VertexBufferGPU=d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(),verts.data(),vbSz,geo->VertexBufferUploader);
    geo->IndexBufferGPU=d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(),idx.data(),ibSz,geo->IndexBufferUploader);
    geo->VertexByteStride=sizeof(Vertex);
    geo->VertexBufferByteSize=vbSz;
    geo->IndexFormat=DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize=ibSz;
    SubmeshGeometry sub; sub.IndexCount=(UINT)idx.size();
    geo->DrawArgs["cube"]=sub;
    mGeometries["cubeGeo"]=std::move(geo);

    auto ri=std::make_unique<RenderItem>();
    XMStoreFloat4x4(&ri->World,XMMatrixIdentity());
    ri->ObjCBIndex=0; ri->Geo=mGeometries["cubeGeo"].get();
    ri->SubMesh="cube"; ri->TextureName=mFallbackTexName;
    ri->Mat.Ambient={0.2f,0.2f,0.5f,1};
    ri->Mat.Diffuse={0.3f,0.5f,1.0f,1};
    ri->Mat.Specular={1,1,1,64};
    mOpaqueRItems.push_back(ri.get());
    mAllRItems.push_back(std::move(ri));
    CreateProceduralTexture(mFallbackTexName);
}

// ════════════════════════════════════════════════════════════════════════════
// BuildLightSphereObjects — заранее создаёт 128 маленьких сфер для point lights.
// Сами point lights остаются в StructuredBuffer, а сферы являются их видимыми
// объектами в сцене.
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildLightSphereObjects()
{
    const std::string sphereTexName = "__light_sphere_white__";
    CreateSolidTexture(sphereTexName, 0xFFFFFFFF);

    const UINT sliceCount = 24;
    const UINT stackCount = 12;
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    verts.reserve((sliceCount + 1) * (stackCount + 1));

    for (UINT i = 0; i <= stackCount; ++i)
    {
        float phi = MathHelper::Pi * (float)i / (float)stackCount;
        float y   = cosf(phi);
        float r   = sinf(phi);

        for (UINT j = 0; j <= sliceCount; ++j)
        {
            float theta = 2.0f * MathHelper::Pi * (float)j / (float)sliceCount;
            XMFLOAT3 p = { r * sinf(theta), y, r * cosf(theta) };
            Vertex v{};
            v.Pos      = p;
            // Нормали направлены внутрь: point light находится в центре сферы,
            // поэтому такая сфера подсвечивается собственным источником.
            v.Normal   = { -p.x, -p.y, -p.z };
            v.TexCoord = { (float)j / (float)sliceCount, (float)i / (float)stackCount };
            verts.push_back(v);
        }
    }

    for (UINT i = 0; i < stackCount; ++i)
    {
        for (UINT j = 0; j < sliceCount; ++j)
        {
            uint32_t a = i * (sliceCount + 1) + j;
            uint32_t b = a + sliceCount + 1;
            idx.push_back(a);     idx.push_back(b);     idx.push_back(a + 1);
            idx.push_back(a + 1); idx.push_back(b);     idx.push_back(b + 1);
        }
    }

    UINT vbSz = (UINT)(verts.size() * sizeof(Vertex));
    UINT ibSz = (UINT)(idx.size()   * sizeof(uint32_t));

    auto geo = std::make_unique<MeshGeometry>();
    geo->Name = "lightSphereGeo";
    ThrowIfFailed(D3DCreateBlob(vbSz, &geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(), verts.data(), vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz, &geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(), idx.data(), ibSz);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), verts.data(), vbSz, geo->VertexBufferUploader);
    geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), idx.data(), ibSz, geo->IndexBufferUploader);
    geo->VertexByteStride     = sizeof(Vertex);
    geo->VertexBufferByteSize = vbSz;
    geo->IndexFormat          = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize  = ibSz;

    SubmeshGeometry sub{};
    sub.IndexCount = (UINT)idx.size();
    geo->DrawArgs["sphere"] = sub;

    MeshGeometry* geoPtr = geo.get();
    const std::string geoName = geo->Name;
    mGeometries[geoName] = std::move(geo);

    mLightSphereRItems.clear();
    mLightSphereRItems.reserve(MaxPointLightObjects);

    for (UINT i = 0; i < MaxPointLightObjects; ++i)
    {
        auto ri = std::make_unique<RenderItem>();
        ri->ObjCBIndex  = (UINT)mAllRItems.size();
        ri->Geo         = geoPtr;
        ri->SubMesh     = "sphere";
        ri->TextureName = sphereTexName;
        ri->Visible     = false;
        ri->Mat.Ambient  = { 0.6f, 0.5f, 0.25f, 1.f };
        ri->Mat.Diffuse  = { 1.0f, 0.85f, 0.35f, 1.f };
        ri->Mat.Specular = { 1.0f, 1.0f, 1.0f, 64.f };
        XMStoreFloat4x4(&ri->World, XMMatrixScaling(0.001f, 0.001f, 0.001f));

        mLightSphereRItems.push_back(ri.get());
        mOpaqueRItems.push_back(ri.get());
        mAllRItems.push_back(std::move(ri));
    }
}

// ════════════════════════════════════════════════════════════════════════════
// BuildLampMeshes — создаёт меши абажуров над Point Light 0 и 1
// Абажур = плоская коробка (диск сверху), тёмно-красная
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildLampMeshes()
{
    // Позиции ламп совпадают с Point Light 0 и 1
    struct LampDesc { float x, y, z; const char* geoName; const char* subName; };
    LampDesc lamps[] = {
        { -1.5f, 3.2f, 0.f, "lampGeo0", "lamp0" },
        {  1.5f, 3.2f, 0.f, "lampGeo1", "lamp1" },
    };

    // Создаём процедурную текстуру абажура (тёмно-красная)
    const std::string lampTexName = "__lamp__";
    if (!mTextures.count(lampTexName))
    {
        const UINT W = 8, H = 8;
        std::vector<UINT> px(W * H, 0xFF2A1A1A); // тёмно-красный
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = W; td.Height = H; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        auto defH = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> tex, upload;
        ThrowIfFailed(md3dDevice->CreateCommittedResource(&defH, D3D12_HEAP_FLAG_NONE,
            &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)));
        UINT64 upSz = 0;
        md3dDevice->GetCopyableFootprints(&td, 0, 1, 0, nullptr, nullptr, nullptr, &upSz);
        auto uplH = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
        auto uplD = CD3DX12_RESOURCE_DESC_BUFFER(upSz);
        ThrowIfFailed(md3dDevice->CreateCommittedResource(&uplH, D3D12_HEAP_FLAG_NONE,
            &uplD, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout; UINT nr; UINT64 rs;
        md3dDevice->GetCopyableFootprints(&td, 0, 1, 0, &layout, &nr, &rs, nullptr);
        BYTE* m = nullptr; upload->Map(0, nullptr, (void**)&m);
        for (UINT r = 0; r < nr; ++r)
            memcpy(m + layout.Offset + r * layout.Footprint.RowPitch,
                   (BYTE*)px.data() + r * W * 4, rs);
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION dst = {tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src = {upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        src.PlacedFootprint = layout;
        mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto b = CD3DX12_RESOURCE_BARRIER_TRANSITION(tex.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        mCommandList->ResourceBarrier(1, &b);
        mTextures[lampTexName] = tex;
        mTextureUploads[lampTexName] = upload;
    }

    // Абажур = плоский ящик (широкий, низкий) — 0.7 × 0.15 × 0.5
    GeometryGenerator gen;

    for (auto& ld : lamps)
    {
        auto box = gen.CreateBox(0.7f, 0.15f, 0.5f, 0);

        std::vector<Vertex> verts(box.Vertices.size());
        for (size_t i = 0; i < box.Vertices.size(); ++i) {
            verts[i].Pos      = box.Vertices[i].Position;
            verts[i].Normal   = box.Vertices[i].Normal;
            verts[i].TexCoord = box.Vertices[i].TexCoord;
        }
        auto idx16 = box.GetIndices16();
        std::vector<uint32_t> idx(idx16.begin(), idx16.end());

        UINT vbSz = (UINT)(verts.size() * sizeof(Vertex));
        UINT ibSz = (UINT)(idx.size()   * sizeof(uint32_t));

        auto geo = std::make_unique<MeshGeometry>();
        geo->Name = ld.geoName;
        ThrowIfFailed(D3DCreateBlob(vbSz, &geo->VertexBufferCPU));
        memcpy(geo->VertexBufferCPU->GetBufferPointer(), verts.data(), vbSz);
        ThrowIfFailed(D3DCreateBlob(ibSz, &geo->IndexBufferCPU));
        memcpy(geo->IndexBufferCPU->GetBufferPointer(), idx.data(), ibSz);
        geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
            mCommandList.Get(), verts.data(), vbSz, geo->VertexBufferUploader);
        geo->IndexBufferGPU  = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
            mCommandList.Get(), idx.data(),   ibSz, geo->IndexBufferUploader);
        geo->VertexByteStride     = sizeof(Vertex);
        geo->VertexBufferByteSize = vbSz;
        geo->IndexFormat          = DXGI_FORMAT_R32_UINT;
        geo->IndexBufferByteSize  = ibSz;

        SubmeshGeometry sub; sub.IndexCount = (UINT)idx.size();
        geo->DrawArgs[ld.subName] = sub;

        auto ri = std::make_unique<RenderItem>();
        // Просто Translation — транспонирование делается в UpdateObjectCBs
        XMStoreFloat4x4(&ri->World,
            XMMatrixTranslation(ld.x, ld.y, ld.z));
        ri->ObjCBIndex  = (UINT)mAllRItems.size();
        ri->Geo         = geo.get();
        ri->SubMesh     = ld.subName;
        ri->TextureName = lampTexName;
        ri->Mat.Ambient  = { 0.05f, 0.02f, 0.02f, 1.f };
        ri->Mat.Diffuse  = { 0.40f, 0.10f, 0.10f, 1.f }; // тёмно-красный абажур
        ri->Mat.Specular = { 0.3f,  0.1f,  0.1f,  8.f };

        mGeometries[ld.geoName] = std::move(geo);
        mOpaqueRItems.push_back(ri.get());
        mAllRItems.push_back(std::move(ri));
    }
}

// ─── Camera ───────────────────────────────────────────────────────────────────
void PhongApp::GetCameraBasis(XMVECTOR& pos, XMVECTOR& forward,
                              XMVECTOR& right, XMVECTOR& up) const
{
    pos = XMLoadFloat3(&mCameraPos);

    float cp = cosf(mPitch);
    XMFLOAT3 f(cp * sinf(mYaw), sinf(mPitch), cp * cosf(mYaw));
    forward = XMVector3Normalize(XMLoadFloat3(&f));
    right   = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), forward));
    up      = XMVector3Normalize(XMVector3Cross(forward, right));
}

void PhongApp::UpdateCamera(const GameTimer& gt)
{
    XMVECTOR pos, forward, right, up;
    GetCameraBasis(pos, forward, right, up);

    float speed = mMoveSpeed * gt.DeltaTime();
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= 2.5f;

    if (GetAsyncKeyState('W') & 0x8000) pos += forward * speed;
    if (GetAsyncKeyState('S') & 0x8000) pos -= forward * speed;
    if (GetAsyncKeyState('D') & 0x8000) pos += right   * speed;
    if (GetAsyncKeyState('A') & 0x8000) pos -= right   * speed;
    if (GetAsyncKeyState('E') & 0x8000) pos += XMVectorSet(0, 1, 0, 0) * speed;
    if (GetAsyncKeyState('Q') & 0x8000) pos -= XMVectorSet(0, 1, 0, 0) * speed;

    XMStoreFloat3(&mCameraPos, pos);
}

void PhongApp::UpdateLightObjects(const GameTimer& gt)
{
    // Границы комнаты breakfast_room. Летящие световые сферы останавливаются
    // на ближайшей стене/полу/потолке и остаются там как point light objects.
    const float minX = -6.6f,  maxX = 5.4f;
    const float minY = -1.74f, maxY = 8.11f;
    const float minZ = -4.77f, maxZ = 10.16f;
    const float sphereRadius = 0.18f;
    const float dt = gt.DeltaTime();

    for (auto& obj : mLightObjects)
    {
        if (!obj.Active || !obj.Moving) continue;

        XMFLOAT3 p = obj.Light.Position;
        p.x += obj.Velocity.x * dt;
        p.y += obj.Velocity.y * dt;
        p.z += obj.Velocity.z * dt;

        bool hit = false;
        if (p.x < minX + sphereRadius) { p.x = minX + sphereRadius; hit = true; }
        if (p.x > maxX - sphereRadius) { p.x = maxX - sphereRadius; hit = true; }
        if (p.y < minY + sphereRadius) { p.y = minY + sphereRadius; hit = true; }
        if (p.y > maxY - sphereRadius) { p.y = maxY - sphereRadius; hit = true; }
        if (p.z < minZ + sphereRadius) { p.z = minZ + sphereRadius; hit = true; }
        if (p.z > maxZ - sphereRadius) { p.z = maxZ - sphereRadius; hit = true; }

        obj.Light.Position = p;
        if (hit)
        {
            obj.Moving = false;
            obj.Velocity = { 0.f, 0.f, 0.f };
        }
    }

    mPointLights.clear();
    mPointLights.reserve(mLightObjects.size());
    for (const auto& obj : mLightObjects)
        if (obj.Active)
            mPointLights.push_back(obj.Light);

    // Синхронизируем видимые сферы с массивом point lights.
    // Порядок сфер совпадает с порядком данных в StructuredBuffer.
    for (UINT i = 0; i < (UINT)mLightSphereRItems.size(); ++i)
    {
        RenderItem* ri = mLightSphereRItems[i];
        if (i < (UINT)mPointLights.size())
        {
            const PointLight& L = mPointLights[i];
            XMFLOAT3 p = L.Position;
            XMStoreFloat4x4(&ri->World,
                XMMatrixScaling(sphereRadius, sphereRadius, sphereRadius) *
                XMMatrixTranslation(p.x, p.y, p.z));

            ri->Visible = true;
            ri->Mat.Ambient  = { L.Diffuse.x * 0.45f, L.Diffuse.y * 0.45f, L.Diffuse.z * 0.45f, 1.f };
            ri->Mat.Diffuse  = { L.Diffuse.x, L.Diffuse.y, L.Diffuse.z, 1.f };
            ri->Mat.Specular = { 1.0f, 1.0f, 1.0f, 64.f };
            ri->NumFramesDirty = 3;
        }
        else
        {
            if (ri->Visible)
            {
                ri->Visible = false;
                XMStoreFloat4x4(&ri->World, XMMatrixScaling(0.001f, 0.001f, 0.001f));
                ri->NumFramesDirty = 3;
            }
        }
    }

    mLightingData.NumPointLights = (int)mPointLights.size();
}

void PhongApp::ShootPointLight()
{
    XMVECTOR eye, forward, right, up;
    GetCameraBasis(eye, forward, right, up);

    // Источник появляется прямо перед камерой и летит строго по forward-вектору.
    // Поэтому при взгляде вверх он летит вверх, при взгляде вниз — вниз.
    XMVECTOR spawn = eye + forward * 0.55f;

    XMFLOAT3 spawnPos, dir;
    XMStoreFloat3(&spawnPos, spawn);
    XMStoreFloat3(&dir, XMVector3Normalize(forward));

    static const XMFLOAT4 kColors[] = {
        { 1.00f, 0.85f, 0.35f, 1.f }, // тёплый жёлтый
        { 0.25f, 0.65f, 1.00f, 1.f }, // голубой
        { 0.30f, 1.00f, 0.35f, 1.f }, // зелёный
        { 1.00f, 0.35f, 0.90f, 1.f }, // розовый
        { 1.00f, 0.45f, 0.20f, 1.f }, // оранжевый
    };
    XMFLOAT4 color = kColors[mShotCounter % (sizeof(kColors) / sizeof(kColors[0]))];

    PointLight L{};
    L.Position    = spawnPos;
    L.Range       = 8.0f;
    L.Ambient     = { color.x * 0.05f, color.y * 0.05f, color.z * 0.05f, 1.f };
    L.Diffuse     = color;
    L.Specular    = color;
    L.Attenuation = { 1.f, 0.16f, 0.030f };

    LightObject obj{};
    obj.Light    = L;
    obj.Active   = true;
    obj.Moving   = true;

    const float shotSpeed = 9.0f;
    obj.Velocity = { dir.x * shotSpeed, dir.y * shotSpeed, dir.z * shotSpeed };

    if (mLightObjects.size() >= MaxPointLightObjects)
    {
        // Сохраняем три стартовых комнатных источника, а удаляем самый старый
        // выстреленный источник.
        if (mLightObjects.size() > StaticPointLightCount)
            mLightObjects.erase(mLightObjects.begin() + StaticPointLightCount);
        else
            mLightObjects.erase(mLightObjects.begin());
    }

    mLightObjects.push_back(obj);
    ++mShotCounter;
}

LRESULT PhongApp::MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_KEYDOWN)
    {
        if (wParam == VK_SPACE && ((lParam & (1 << 30)) == 0))
        {
            ShootPointLight();
            return 0;
        }
    }
    return D3DApp::MsgProc(hwnd, msg, wParam, lParam);
}

void PhongApp::OnMouseDown(WPARAM,int x,int y){mLastMousePos={x,y};SetCapture(mhMainWnd);}
void PhongApp::OnMouseUp(WPARAM,int,int){ReleaseCapture();}
void PhongApp::OnMouseMove(WPARAM b,int x,int y)
{
    if (b&MK_LBUTTON){
        mYaw   += XMConvertToRadians(0.20f*(x-mLastMousePos.x));
        mPitch += XMConvertToRadians(-0.20f*(y-mLastMousePos.y));
        mPitch = MathHelper::Clamp(mPitch, -XM_PIDIV2 + 0.05f, XM_PIDIV2 - 0.05f);
    }
    mLastMousePos={x,y};
}
