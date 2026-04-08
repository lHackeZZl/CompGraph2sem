// PhongApp.cpp — Multi-texture OBJ scene loader
#include "PhongApp.h"
#include <fstream>
#include <sstream>
#include <comdef.h>
#include <wincodec.h>
#pragma comment(lib,"windowscodecs.lib")

// ─── FrameResource ────────────────────────────────────────────────────────────
FrameResource::FrameResource(ID3D12Device* d, UINT pass, UINT obj)
{
    ThrowIfFailed(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&CmdListAlloc)));
    PassCB  = std::make_unique<UploadBuffer<CBPerPass  >>(d, pass, true);
    ObjectCB= std::make_unique<UploadBuffer<CBPerObject>>(d, obj,  true);
}

// ─── PhongApp ─────────────────────────────────────────────────────────────────
PhongApp::PhongApp(HINSTANCE h) : D3DApp(h)
{ mMainWndCaption = L"DX12 — Breakfast Room"; }

PhongApp::~PhongApp() { if (md3dDevice) FlushCommandQueue(); }

bool PhongApp::Initialize()
{
    if (!D3DApp::Initialize()) return false;
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    BuildRootSignature();
    BuildShadersAndInputLayout();

    // Пытаемся загрузить сцену model.obj / breakfast_room.obj
    bool sceneLoaded = LoadScene("breakfast_room.obj");
    if (!sceneLoaded) sceneLoaded = LoadScene("model.obj");

    // Если сцена не загружена — рисуем дефолтный куб
    if (!sceneLoaded) BuildDefaultCube();

    // Включаем вертексную анимацию только на картине и светильниках
    // Проверяем имена материалов по ключевым словам
    // Точные имена материалов из breakfast_room.mtl
    // Анимируем: картину (Artwork) + лампы (Chrome, Frosted_Glass, Gold_Paint)
    static const std::vector<std::string> kAnimMaterials = {
        "breakfast_room:Artwork",
        "breakfast_room:Chrome",
        "breakfast_room:Frosted_Glass",
        "breakfast_room:Gold_Paint",
    };
    for (auto& ri : mAllRItems) {
        for (const auto& animMat : kAnimMaterials) {
            if (ri->SubMesh == animMat) {
                ri->AnimEnabled    = true;
                ri->NumFramesDirty = 3;
                break;
            }
        }
    }

    // Если ни одной текстуры нет — создаём процедурную шахматную
    if (mTextures.empty()) CreateProceduralTexture(mFallbackTexName);

    BuildFrameResources();
    BuildDescriptorHeaps();
    BuildConstantBufferViews();
    BuildTextureViews();
    BuildPSO();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, c);
    FlushCommandQueue();
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// RootSignature: slot0=CBV obj(b0), slot1=CBV pass(b1), slot2=SRV tex(t0)
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE r0 = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0);
    D3D12_DESCRIPTOR_RANGE r1 = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,1);
    D3D12_DESCRIPTOR_RANGE r2 = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0);
    D3D12_ROOT_PARAMETER p[3];
    p[0] = CD3DX12_ROOT_PARAMETER_TABLE(1,&r0);
    p[1] = CD3DX12_ROOT_PARAMETER_TABLE(1,&r1);
    p[2] = CD3DX12_ROOT_PARAMETER_TABLE(1,&r2,D3D12_SHADER_VISIBILITY_PIXEL);

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter   = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MipLODBias = 0; samp.MaxAnisotropy = 1;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3; desc.pParameters = p;
    desc.NumStaticSamplers = 1; desc.pStaticSamplers = &samp;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> ser, err;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &ser, &err);
    if (err) OutputDebugStringA((char*)err->GetBufferPointer());
    ThrowIfFailed(hr);
    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        ser->GetBufferPointer(), ser->GetBufferSize(), IID_PPV_ARGS(&mRootSignature)));
}

void PhongApp::BuildShadersAndInputLayout()
{
    mVsByteCode = d3dUtil::CompileShader(L"phong.hlsl", nullptr, "VS", "vs_5_1");
    mPsByteCode = d3dUtil::CompileShader(L"phong.hlsl", nullptr, "PS", "ps_5_1");
    mInputLayout = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,   0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
}

// ════════════════════════════════════════════════════════════════════════════
// Загрузка текстуры через WIC (JPG / PNG / BMP)
// ════════════════════════════════════════════════════════════════════════════
bool PhongApp::LoadTexture(const std::string& name, const std::wstring& path)
{
    if (mTextures.count(name)) return true; // уже загружена
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic)))) return false;

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
    conv->CopyPixels(nullptr, W * 4, (UINT)pixels.size(), pixels.data());

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
               pixels.data() + r * W * 4, rowSize);
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = { tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src = { upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    src.PlacedFootprint = layout;
    mCommandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    auto b = CD3DX12_RESOURCE_BARRIER_TRANSITION(tex.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1, &b);

    mTextures[name]       = tex;
    mTextureUploads[name] = upload;
    return true;
}

void PhongApp::CreateProceduralTexture(const std::string& name)
{
    const UINT W = 256, H = 256;
    std::vector<UINT> px(W * H);
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x)
            px[y*W+x] = ((x/32 + y/32) % 2 == 0) ? 0xFF4488FF : 0xFF112266;

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
    mTextures[name]       = tex;
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
    ObjMaterial* cur = nullptr;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line); std::string tok; ss >> tok;
        if      (tok=="newmtl"){ std::string n; ss>>n; mats[n]={}; mats[n].Name=n; cur=&mats[n]; }
        else if (cur) {
            if      (tok=="Ka") { ss>>cur->Ambient.x>>cur->Ambient.y>>cur->Ambient.z; }
            else if (tok=="Kd") { ss>>cur->Diffuse.x>>cur->Diffuse.y>>cur->Diffuse.z; }
            else if (tok=="Ks") { ss>>cur->Specular.x>>cur->Specular.y>>cur->Specular.z; }
            else if (tok=="Ns") { ss>>cur->Specular.w; }
            else if (tok=="map_Kd") { ss>>cur->DiffuseMap; }
        }
    }
    return mats;
}

// ════════════════════════════════════════════════════════════════════════════
// OBJ loader — группирует вершины по материалам, один RenderItem на группу
// ════════════════════════════════════════════════════════════════════════════
bool PhongApp::LoadScene(const std::string& objFile)
{
    std::ifstream f(objFile); if (!f) return false;

    // Директория файла (для поиска текстур рядом)
    std::string dir;
    auto sl = objFile.rfind('/');
    if (sl == std::string::npos) sl = objFile.rfind('\\');
    if (sl != std::string::npos) dir = objFile.substr(0, sl+1);

    std::vector<XMFLOAT3> pos, nrm;
    std::vector<XMFLOAT2> uvs;

    // Группа на материал
    struct MatGroup {
        ObjMaterial mat;
        std::vector<Vertex>   verts;
        std::vector<uint32_t> idx;
    };
    std::unordered_map<std::string, MatGroup> groups;
    std::unordered_map<std::string, ObjMaterial> mats;
    std::string curMat = "__default__";
    groups[curMat] = {};

    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line); std::string tok; ss >> tok;
        if (tok=="v")  { XMFLOAT3 p; ss>>p.x>>p.y>>p.z; pos.push_back(p); }
        else if (tok=="vn") { XMFLOAT3 n; ss>>n.x>>n.y>>n.z; nrm.push_back(n); }
        else if (tok=="vt") { XMFLOAT2 u; ss>>u.x>>u.y; u.y=1.f-u.y; uvs.push_back(u); }
        else if (tok=="mtllib") {
            std::string mf; ss>>mf;
            mats = LoadMtl(dir + mf);
        }
        else if (tok=="usemtl") {
            ss >> curMat;
            if (!groups.count(curMat)) {
                groups[curMat] = {};
                if (mats.count(curMat)) groups[curMat].mat = mats[curMat];
            }
        }
        else if (tok=="f") {
            // Разбираем грань, триангулируем (fan)
            std::vector<std::tuple<int,int,int>> face;
            std::string fc;
            while (ss >> fc) {
                int vi=0, ti=-1, ni=-1;
                auto s1 = fc.find('/');
                vi = std::stoi(fc.substr(0, s1)) - 1;
                if (s1 != std::string::npos) {
                    auto s2 = fc.find('/', s1+1);
                    if (s2 != s1+1 && s2 != std::string::npos)
                        ti = std::stoi(fc.substr(s1+1, s2-s1-1)) - 1;
                    else if (s1+1 < fc.size() && fc[s1+1]!='/')
                        ti = std::stoi(fc.substr(s1+1)) - 1;
                    if (s2 != std::string::npos && s2+1 < fc.size())
                        ni = std::stoi(fc.substr(s2+1)) - 1;
                }
                face.push_back({vi, ti, ni});
            }
            auto& g = groups[curMat];
            for (size_t i = 1; i+1 < face.size(); ++i) {
                for (auto fv : {face[0], face[i], face[i+1]}) {
                    Vertex vtx{};
                    int vi=std::get<0>(fv), ti=std::get<1>(fv), ni=std::get<2>(fv);
                    if (vi >= 0 && vi < (int)pos.size()) vtx.Pos = pos[vi];
                    if (ni >= 0 && ni < (int)nrm.size()) vtx.Normal = nrm[ni];
                    if (ti >= 0 && ti < (int)uvs.size()) vtx.TexCoord = uvs[ti];
                    g.idx.push_back((uint32_t)g.verts.size());
                    g.verts.push_back(vtx);
                }
            }
        }
    }

    if (groups.empty()) return false;

    // Создаём одну MeshGeometry со всеми submesh
    auto geo = std::make_unique<MeshGeometry>(); geo->Name = "sceneGeo";
    std::vector<Vertex>   allVerts;
    std::vector<uint32_t> allIdx;

    for (auto& [matName, g] : groups) {
        if (g.verts.empty()) continue;

        SubmeshGeometry sub;
        sub.IndexCount         = (UINT)g.idx.size();
        sub.StartIndexLocation = (UINT)allIdx.size();
        sub.BaseVertexLocation = (INT)allVerts.size();
        geo->DrawArgs[matName] = sub;

        for (auto& v : g.verts) allVerts.push_back(v);
        for (auto  i : g.idx)   allIdx.push_back(i);

        // Загружаем текстуру этого материала
        std::string texName = mFallbackTexName;
        if (!g.mat.DiffuseMap.empty()) {
            texName = g.mat.DiffuseMap;
            if (!mTextures.count(texName)) {
                // Пробуем загрузить из текущей директории
                std::wstring wpath(dir.begin(), dir.end());
                std::wstring wname(g.mat.DiffuseMap.begin(), g.mat.DiffuseMap.end());
                if (!LoadTexture(texName, wpath + wname))
                    texName = mFallbackTexName; // откат на шахматную
            }
        }

        // RenderItem для этой группы
        auto ri = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&ri->World, XMMatrixIdentity());
        ri->ObjCBIndex  = (UINT)mAllRItems.size();
        ri->Geo         = geo.get();
        ri->SubMesh     = matName;
        ri->TextureName = texName;
        ri->Mat         = g.mat;
        mOpaqueRItems.push_back(ri.get());
        mAllRItems.push_back(std::move(ri));
    }

    if (allVerts.empty()) return false;

    UINT vbSz = (UINT)(allVerts.size() * sizeof(Vertex));
    UINT ibSz = (UINT)(allIdx.size()   * sizeof(uint32_t));

    ThrowIfFailed(D3DCreateBlob(vbSz, &geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(), allVerts.data(), vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz, &geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(), allIdx.data(), ibSz);

    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), allVerts.data(), vbSz, geo->VertexBufferUploader);
    geo->IndexBufferGPU  = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), allIdx.data(),   ibSz, geo->IndexBufferUploader);

    geo->VertexByteStride     = sizeof(Vertex);
    geo->VertexBufferByteSize = vbSz;
    geo->IndexFormat          = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize  = ibSz;

    mGeometries["sceneGeo"] = std::move(geo);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Дефолтный куб (если OBJ не найден)
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildDefaultCube()
{
    GeometryGenerator g;
    auto cube = g.CreateBox(1.5f, 1.5f, 1.5f, 0);
    std::vector<Vertex> verts(cube.Vertices.size());
    for (size_t i = 0; i < cube.Vertices.size(); ++i) {
        verts[i].Pos      = cube.Vertices[i].Position;
        verts[i].Normal   = cube.Vertices[i].Normal;
        verts[i].TexCoord = cube.Vertices[i].TexCoord;
    }
    auto idx16 = cube.GetIndices16();
    std::vector<uint32_t> idx(idx16.begin(), idx16.end());
    UINT vbSz = (UINT)(verts.size()*sizeof(Vertex));
    UINT ibSz = (UINT)(idx.size()*sizeof(uint32_t));

    auto geo = std::make_unique<MeshGeometry>(); geo->Name = "cubeGeo";
    ThrowIfFailed(D3DCreateBlob(vbSz, &geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(), verts.data(), vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz, &geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(), idx.data(), ibSz);
    geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), verts.data(), vbSz, geo->VertexBufferUploader);
    geo->IndexBufferGPU  = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
        mCommandList.Get(), idx.data(), ibSz, geo->IndexBufferUploader);
    geo->VertexByteStride     = sizeof(Vertex);
    geo->VertexBufferByteSize = vbSz;
    geo->IndexFormat          = DXGI_FORMAT_R32_UINT;
    geo->IndexBufferByteSize  = ibSz;
    SubmeshGeometry sub; sub.IndexCount = (UINT)idx.size();
    geo->DrawArgs["cube"] = sub;
    mGeometries["cubeGeo"] = std::move(geo);

    auto ri = std::make_unique<RenderItem>();
    XMStoreFloat4x4(&ri->World, XMMatrixIdentity());
    ri->ObjCBIndex  = 0;
    ri->Geo         = mGeometries["cubeGeo"].get();
    ri->SubMesh     = "cube";
    ri->TextureName = mFallbackTexName;
    ri->Mat.Ambient  = {0.2f,0.2f,0.5f,1};
    ri->Mat.Diffuse  = {0.3f,0.5f,1.0f,1};
    ri->Mat.Specular = {1,1,1,64};
    mOpaqueRItems.push_back(ri.get());
    mAllRItems.push_back(std::move(ri));
    CreateProceduralTexture(mFallbackTexName);
}

void PhongApp::BuildFrameResources()
{
    for (int i = 0; i < 3; ++i)
        mFrameResources.push_back(std::make_unique<FrameResource>(
            md3dDevice.Get(), 1, (UINT)mAllRItems.size()));
}

// ════════════════════════════════════════════════════════════════════════════
// Descriptor Heap: [objCBVs][passCBVs][textureSRVs]
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildDescriptorHeaps()
{
    UINT n    = (UINT)mAllRItems.size();
    UINT texN = (UINT)mTextures.size();

    mPassCbvOffset = n * 3;
    mSrvBaseOffset = (n + 1) * 3;

    D3D12_DESCRIPTOR_HEAP_DESC d = {};
    d.NumDescriptors = mSrvBaseOffset + texN;
    d.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&d, IID_PPV_ARGS(&mCbvSrvHeap)));
}

void PhongApp::BuildConstantBufferViews()
{
    UINT objSz  = d3dUtil::CalcConstantBufferByteSize(sizeof(CBPerObject));
    UINT passSz = d3dUtil::CalcConstantBufferByteSize(sizeof(CBPerPass));
    UINT n      = (UINT)mAllRItems.size();

    for (int frame = 0; frame < 3; ++frame) {
        auto cb = mFrameResources[frame]->ObjectCB->Resource();
        for (UINT i = 0; i < n; ++i) {
            auto h = CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                frame*n+i, mCbvSrvUavDescriptorSize);
            D3D12_CONSTANT_BUFFER_VIEW_DESC v = {
                cb->GetGPUVirtualAddress() + (UINT64)i*objSz, objSz };
            md3dDevice->CreateConstantBufferView(&v, h);
        }
    }
    for (int frame = 0; frame < 3; ++frame) {
        auto h = CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
            mPassCbvOffset+frame, mCbvSrvUavDescriptorSize);
        auto cb = mFrameResources[frame]->PassCB->Resource();
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

void PhongApp::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = {};
    d.InputLayout    = { mInputLayout.data(), (UINT)mInputLayout.size() };
    d.pRootSignature = mRootSignature.Get();
    d.VS = { mVsByteCode->GetBufferPointer(), mVsByteCode->GetBufferSize() };
    d.PS = { mPsByteCode->GetBufferPointer(), mPsByteCode->GetBufferSize() };
    D3D12_RASTERIZER_DESC rastDesc = CD3DX12_RASTERIZER_DESC_DEFAULT();
    rastDesc.CullMode = D3D12_CULL_MODE_NONE; // отключаем: проволочные стулья имеют грани внутрь
    d.RasterizerState = rastDesc;
    d.BlendState        = CD3DX12_BLEND_DESC_DEFAULT();
    d.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
    d.SampleMask        = UINT_MAX;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets  = 1; d.RTVFormats[0] = mBackBufferFormat;
    d.SampleDesc.Count  = m4xMsaaState ? 4 : 1;
    d.SampleDesc.Quality= m4xMsaaState ? (m4xMsaaQuality-1) : 0;
    d.DSVFormat         = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&mPSO)));
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::OnResize() { D3DApp::OnResize(); }

void PhongApp::Update(const GameTimer& gt)
{
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex+1) % 3;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();
    if (mCurrFrameResource->Fence != 0 &&
        mFence->GetCompletedValue() < mCurrFrameResource->Fence) {
        HANDLE ev = CreateEventEx(nullptr,nullptr,0,EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, ev));
        WaitForSingleObject(ev, INFINITE); CloseHandle(ev);
    }
    mUVOffset += gt.DeltaTime() * 0.05f;
    if (mUVOffset > 1.f) mUVOffset -= 1.f;
    mTime += gt.DeltaTime();
    UpdateObjectCBs(gt);
    UpdatePassCB(gt);
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
        o.MatAmbient   = e->Mat.Ambient;
        o.MatDiffuse   = e->Mat.Diffuse;
        o.MatSpecular  = e->Mat.Specular;
        o.AnimEnabled  = e->AnimEnabled ? 1.0f : 0.0f;
        cb->CopyData(e->ObjCBIndex, o);
        --e->NumFramesDirty;
    }
}

void PhongApp::UpdatePassCB(const GameTimer&)
{
    float x = mRadius*sinf(mPhi)*cosf(mTheta);
    float y = mRadius*cosf(mPhi);
    float z = mRadius*sinf(mPhi)*sinf(mTheta);
    XMVECTOR eye = XMVectorSet(x,y,z,1.f);
    XMStoreFloat4x4(&mMainPassCB.View,
        XMMatrixTranspose(XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0,1,0,0))));
    XMStoreFloat4x4(&mMainPassCB.Proj,
        XMMatrixTranspose(XMLoadFloat4x4(&mProj)));
    XMStoreFloat3(&mMainPassCB.EyePosW, eye);

    mMainPassCB.LightDir      = { 0.6f, -0.5f, 0.6f };
    mMainPassCB.LightAmbient  = { 0.35f, 0.35f, 0.35f, 1.f };
    mMainPassCB.LightDiffuse  = { 0.8f,  0.8f,  0.8f,  1.f };
    mMainPassCB.LightSpecular = { 1.f,   1.f,   1.f,   1.f };
    mMainPassCB.TexScale  = { 1.f, 1.f };
    mMainPassCB.TexOffset = { 0.f, 0.f }; // без UV-анимации для сцены
    mMainPassCB.Time      = mTime;

    mCurrFrameResource->PassCB->CopyData(0, mMainPassCB);
}

// ════════════════════════════════════════════════════════════════════════════
// Draw
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::Draw(const GameTimer&)
{
    auto alloc = mCurrFrameResource->CmdListAlloc;
    ThrowIfFailed(alloc->Reset());
    ThrowIfFailed(mCommandList->Reset(alloc.Get(), mPSO.Get()));
    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    auto b1 = CD3DX12_RESOURCE_BARRIER_TRANSITION(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1, &b1);

    const float cc[4] = { 0.08f,0.08f,0.12f,1.f };
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), cc, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL, 1.f, 0, 0, nullptr);
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    ID3D12DescriptorHeap* heaps[] = { mCbvSrvHeap.Get() };
    mCommandList->SetDescriptorHeaps(1, heaps);
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    // Per-pass CBV → slot 1
    auto passH = CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mPassCbvOffset + mCurrFrameResourceIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, passH);

    DrawRenderItems(mCommandList.Get());

    auto b2 = CD3DX12_RESOURCE_BARRIER_TRANSITION(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1, &b2);
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(1, c);
    ThrowIfFailed(mSwapChain->Present(0, 0));
    mCurrBackBuffer = (mCurrBackBuffer+1) % 2;
    mCurrFrameResource->Fence = ++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void PhongApp::DrawRenderItems(ID3D12GraphicsCommandList* cmd)
{
    UINT n = (UINT)mAllRItems.size();
    for (auto* ri : mOpaqueRItems) {
        // Per-object CBV → slot 0
        auto objH = CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            mCurrFrameResourceIndex*n + ri->ObjCBIndex, mCbvSrvUavDescriptorSize);
        cmd->SetGraphicsRootDescriptorTable(0, objH);

        // Texture SRV → slot 2 (берём по имени текстуры этого RenderItem)
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

// ─── Camera ───────────────────────────────────────────────────────────────────
void PhongApp::OnMouseDown(WPARAM,int x,int y){mLastMousePos={x,y};SetCapture(mhMainWnd);}
void PhongApp::OnMouseUp(WPARAM,int,int){ReleaseCapture();}
void PhongApp::OnMouseMove(WPARAM b,int x,int y)
{
    if (b & MK_LBUTTON) {
        mTheta += XMConvertToRadians(0.25f*(x-mLastMousePos.x));
        mPhi    = MathHelper::Clamp(mPhi+XMConvertToRadians(0.25f*(y-mLastMousePos.y)),0.1f,XM_PI-0.1f);
    } else if (b & MK_RBUTTON) {
        mRadius = MathHelper::Clamp(mRadius + 0.01f*((x-mLastMousePos.x)-(y-mLastMousePos.y)), 0.5f, 50.f);
    }
    mLastMousePos = {x,y};
}
