// PhongApp.cpp — Phong + Texture + Tiling + UV Animation + OBJ+MTL loader
#include "PhongApp.h"
#include <fstream>
#include <sstream>
#include <comdef.h>
#include <wincodec.h>  // WIC for image loading
#pragma comment(lib,"windowscodecs.lib")

FrameResource::FrameResource(ID3D12Device* d, UINT pass, UINT obj)
{
    ThrowIfFailed(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&CmdListAlloc)));
    PassCB  =std::make_unique<UploadBuffer<CBPerPass  >>(d,pass,true);
    ObjectCB=std::make_unique<UploadBuffer<CBPerObject>>(d,obj, true);
}

PhongApp::PhongApp(HINSTANCE h):D3DApp(h){mMainWndCaption=L"DX12 Phong + Texture + OBJ";}
PhongApp::~PhongApp(){if(md3dDevice)FlushCommandQueue();}

bool PhongApp::Initialize()
{
    if(!D3DApp::Initialize()) return false;
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(),nullptr));

    BuildRootSignature();
    BuildShadersAndInputLayout();

    // Строим куб как запасной вариант
    BuildGeometry();
    BuildRenderItems();

    // Если есть model.obj — загружаем его и убираем куб из отрисовки
    if(LoadObjModel("model.obj"))
    {
        mOpaqueRItems.clear();
        mOpaqueRItems.push_back(mAllRItems.back().get());
    }

    // Текстура: png/bmp или процедурная шахматная
    if(!LoadTextureFromFile(L"texture.png") &&
       !LoadTextureFromFile(L"texture.bmp"))
        CreateProceduralTexture();
    BuildFrameResources();
    BuildDescriptorHeaps();
    BuildConstantBufferViews();
    CreateTextureViews();
    BuildPSO();

    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[]={mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1,c);
    FlushCommandQueue();
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// RootSignature: slot0=CBV(b0), slot1=CBV(b1), slot2=SRV(t0)
// + static sampler
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildRootSignature()
{
    D3D12_DESCRIPTOR_RANGE r0 = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,0);
    D3D12_DESCRIPTOR_RANGE r1 = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_CBV,1,1);
    D3D12_DESCRIPTOR_RANGE r2 = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0);
    D3D12_ROOT_PARAMETER p[3];
    p[0]=CD3DX12_ROOT_PARAMETER_TABLE(1,&r0);
    p[1]=CD3DX12_ROOT_PARAMETER_TABLE(1,&r1);
    p[2]=CD3DX12_ROOT_PARAMETER_TABLE(1,&r2,D3D12_SHADER_VISIBILITY_PIXEL);

    // Static sampler (wrap + linear)
    D3D12_STATIC_SAMPLER_DESC samp={};
    samp.Filter  =D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU=samp.AddressV=samp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samp.MipLODBias=0; samp.MaxAnisotropy=1;
    samp.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    samp.MaxLOD=D3D12_FLOAT32_MAX;
    samp.ShaderRegister=0; samp.RegisterSpace=0;
    samp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc={};
    desc.NumParameters=3; desc.pParameters=p;
    desc.NumStaticSamplers=1; desc.pStaticSamplers=&samp;
    desc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> ser,err;
    HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&ser,&err);
    if(err) OutputDebugStringA((char*)err->GetBufferPointer());
    ThrowIfFailed(hr);
    ThrowIfFailed(md3dDevice->CreateRootSignature(0,
        ser->GetBufferPointer(),ser->GetBufferSize(),IID_PPV_ARGS(&mRootSignature)));
}

void PhongApp::BuildShadersAndInputLayout()
{
    mVsByteCode=d3dUtil::CompileShader(L"phong.hlsl",nullptr,"VS","vs_5_1");
    mPsByteCode=d3dUtil::CompileShader(L"phong.hlsl",nullptr,"PS","ps_5_1");
    mInputLayout={
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0, 0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"NORMAL",  0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,   0,24,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    };
}

// ════════════════════════════════════════════════════════════════════════════
// Procedural checkerboard texture (fallback if no image file)
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::CreateProceduralTexture()
{
    const UINT W=256, H=256;
    std::vector<UINT> pixels(W*H);
    for(UINT y=0;y<H;++y)
        for(UINT x=0;x<W;++x){
            bool check=((x/32)+(y/32))%2==0;
            // blue checkerboard: light blue vs dark blue
            pixels[y*W+x] = check ? 0xFF4488FF : 0xFF112266;
        }

    D3D12_RESOURCE_DESC texDesc={};
    texDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width=W; texDesc.Height=H; texDesc.DepthOrArraySize=1; texDesc.MipLevels=1;
    texDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; texDesc.SampleDesc.Count=1;
    texDesc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; texDesc.Flags=D3D12_RESOURCE_FLAG_NONE;

    auto defHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&defHeap,D3D12_HEAP_FLAG_NONE,
        &texDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&mTexture)));

    UINT64 uploadSize=0;
    md3dDevice->GetCopyableFootprints(&texDesc,0,1,0,nullptr,nullptr,nullptr,&uploadSize);

    auto uplHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uplDesc=CD3DX12_RESOURCE_DESC_BUFFER(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uplHeap,D3D12_HEAP_FLAG_NONE,
        &uplDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&mTextureUpload)));

    D3D12_SUBRESOURCE_DATA subData={};
    subData.pData=pixels.data();
    subData.RowPitch=W*4;
    subData.SlicePitch=W*H*4;

    // UpdateSubresources helper (manual implementation)
    UINT64 intermediateOffset=0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSize, totalSize;
    md3dDevice->GetCopyableFootprints(&texDesc,0,1,0,&layout,&numRows,&rowSize,&totalSize);

    BYTE* mapped=nullptr;
    mTextureUpload->Map(0,nullptr,(void**)&mapped);
    for(UINT r=0;r<numRows;++r)
        memcpy(mapped+layout.Offset+r*layout.Footprint.RowPitch,
               (BYTE*)subData.pData+r*subData.RowPitch, rowSize);
    mTextureUpload->Unmap(0,nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst={mTexture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex=0;
    D3D12_TEXTURE_COPY_LOCATION src={mTextureUpload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint=layout;
    mCommandList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);

    auto b=CD3DX12_RESOURCE_BARRIER_TRANSITION(mTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1,&b);
}

// ════════════════════════════════════════════════════════════════════════════
// Load texture from PNG/BMP via WIC
// ════════════════════════════════════════════════════════════════════════════
bool PhongApp::LoadTextureFromFile(const std::wstring& path)
{
    // Check file exists
    if(GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES) return false;

    ComPtr<IWICImagingFactory> wic;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic)))) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    if(FAILED(wic->CreateDecoderFromFilename(path.c_str(),nullptr,
        GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    if(FAILED(decoder->GetFrame(0,&frame))) return false;

    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    conv->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,nullptr,0.f,WICBitmapPaletteTypeCustom);

    UINT W,H; conv->GetSize(&W,&H);
    std::vector<BYTE> pixels(W*H*4);
    conv->CopyPixels(nullptr,W*4,(UINT)pixels.size(),pixels.data());

    D3D12_RESOURCE_DESC texDesc={};
    texDesc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width=W; texDesc.Height=H; texDesc.DepthOrArraySize=1; texDesc.MipLevels=1;
    texDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; texDesc.SampleDesc.Count=1;
    texDesc.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;

    auto defHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&defHeap,D3D12_HEAP_FLAG_NONE,
        &texDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&mTexture)));

    UINT64 uploadSize=0;
    md3dDevice->GetCopyableFootprints(&texDesc,0,1,0,nullptr,nullptr,nullptr,&uploadSize);
    auto uplHeap=CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uplDesc=CD3DX12_RESOURCE_DESC_BUFFER(uploadSize);
    ThrowIfFailed(md3dDevice->CreateCommittedResource(&uplHeap,D3D12_HEAP_FLAG_NONE,
        &uplDesc,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&mTextureUpload)));

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout;
    UINT numRows; UINT64 rowSize, totalSize;
    md3dDevice->GetCopyableFootprints(&texDesc,0,1,0,&layout,&numRows,&rowSize,&totalSize);

    BYTE* mapped=nullptr;
    mTextureUpload->Map(0,nullptr,(void**)&mapped);
    for(UINT r=0;r<numRows;++r)
        memcpy(mapped+layout.Offset+r*layout.Footprint.RowPitch,
               pixels.data()+r*W*4, rowSize);
    mTextureUpload->Unmap(0,nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst={mTexture.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.SubresourceIndex=0;
    D3D12_TEXTURE_COPY_LOCATION src={mTextureUpload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint=layout;
    mCommandList->CopyTextureRegion(&dst,0,0,0,&src,nullptr);

    auto b=CD3DX12_RESOURCE_BARRIER_TRANSITION(mTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    mCommandList->ResourceBarrier(1,&b);
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// OBJ+MTL loader
// ════════════════════════════════════════════════════════════════════════════
std::unordered_map<std::string,ObjMaterial>
PhongApp::LoadMtlFile(const std::string& path)
{
    std::unordered_map<std::string,ObjMaterial> mats;
    std::ifstream f(path); if(!f) return mats;
    ObjMaterial* cur=nullptr;
    std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line); std::string tok; ss>>tok;
        if(tok=="newmtl"){
            std::string n; ss>>n; mats[n]={}; mats[n].Name=n; cur=&mats[n];
        } else if(cur){
            if(tok=="Ka"){ ss>>cur->Ambient.x>>cur->Ambient.y>>cur->Ambient.z; }
            else if(tok=="Kd"){ ss>>cur->Diffuse.x>>cur->Diffuse.y>>cur->Diffuse.z; }
            else if(tok=="Ks"){ ss>>cur->Specular.x>>cur->Specular.y>>cur->Specular.z; }
            else if(tok=="Ns"){ ss>>cur->Specular.w; }
            else if(tok=="map_Kd"){ ss>>cur->DiffuseMap; }
        }
    }
    return mats;
}

bool PhongApp::LoadObjModel(const std::string& objFile)
{
    std::ifstream f(objFile); if(!f) return false;

    std::vector<XMFLOAT3> pos,nrm;
    std::vector<XMFLOAT2> uvs;
    std::vector<Vertex>   verts;
    std::vector<uint16_t> idx;

    std::unordered_map<std::string,ObjMaterial> mats;
    ObjMaterial curMat;

    std::string line;
    while(std::getline(f,line)){
        std::istringstream ss(line); std::string tok; ss>>tok;
        if(tok=="v" ){XMFLOAT3 p;ss>>p.x>>p.y>>p.z;pos.push_back(p);}
        else if(tok=="vn"){XMFLOAT3 n;ss>>n.x>>n.y>>n.z;nrm.push_back(n);}
        else if(tok=="vt"){XMFLOAT2 u;ss>>u.x>>u.y;u.y=1.f-u.y;uvs.push_back(u);}
        else if(tok=="mtllib"){
            std::string mf; ss>>mf;
            // extract dir
            auto sl=objFile.rfind('/'); if(sl==std::string::npos)sl=objFile.rfind('\\');
            std::string dir=sl!=std::string::npos?objFile.substr(0,sl+1):"";
            mats=LoadMtlFile(dir+mf);
        }
        else if(tok=="usemtl"){std::string n;ss>>n;if(mats.count(n))curMat=mats[n];}
        else if(tok=="f"){
            // triangulate, support v/vt/vn  v//vn  v
            std::vector<std::tuple<int,int,int>> face;
            std::string fc;
            while(ss>>fc){
                int vi=0,ti=-1,ni=-1;
                auto s1=fc.find('/');
                vi=std::stoi(fc.substr(0,s1))-1;
                if(s1!=std::string::npos){
                    auto s2=fc.find('/',s1+1);
                    if(s2!=s1+1) ti=std::stoi(fc.substr(s1+1,s2-s1-1))-1;
                    if(s2!=std::string::npos) ni=std::stoi(fc.substr(s2+1))-1;
                }
                face.push_back({vi,ti,ni});
            }
            // fan triangulate
            for(size_t i=1;i+1<face.size();++i){
                for(auto fv:{face[0],face[i],face[i+1]}){
                    Vertex vtx{};
                    vtx.Pos=pos[std::get<0>(fv)];
                    vtx.Normal=std::get<2>(fv)>=0?nrm[std::get<2>(fv)]:XMFLOAT3(0,1,0);
                    vtx.TexCoord=std::get<1>(fv)>=0?uvs[std::get<1>(fv)]:XMFLOAT2(0,0);
                    idx.push_back((uint16_t)verts.size());
                    verts.push_back(vtx);
                }
            }
        }
    }
    if(verts.empty()) return false;

    UINT vbSz=(UINT)(verts.size()*sizeof(Vertex));
    UINT ibSz=(UINT)(idx.size()*sizeof(uint16_t));
    auto geo=std::make_unique<MeshGeometry>(); geo->Name="objGeo";
    ThrowIfFailed(D3DCreateBlob(vbSz,&geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(),verts.data(),vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz,&geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(),idx.data(),ibSz);
    geo->VertexBufferGPU=d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),mCommandList.Get(),verts.data(),vbSz,geo->VertexBufferUploader);
    geo->IndexBufferGPU =d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),mCommandList.Get(),idx.data(),  ibSz,geo->IndexBufferUploader);
    geo->VertexByteStride=sizeof(Vertex); geo->VertexBufferByteSize=vbSz;
    geo->IndexFormat=DXGI_FORMAT_R16_UINT; geo->IndexBufferByteSize=ibSz;
    SubmeshGeometry sub; sub.IndexCount=(UINT)idx.size();
    geo->DrawArgs["obj"]=sub;
    mGeometries["objGeo"]=std::move(geo);

    auto ri=std::make_unique<RenderItem>();
    XMStoreFloat4x4(&ri->World,XMMatrixIdentity());
    ri->ObjCBIndex=(UINT)mAllRItems.size();
    ri->Geo=mGeometries["objGeo"].get();
    ri->IndexCount=ri->Geo->DrawArgs["obj"].IndexCount;
    ri->Mat=curMat;
    mOpaqueRItems.push_back(ri.get());
    mAllRItems.push_back(std::move(ri));
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
// Geometry — cube
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildGeometry()
{
    GeometryGenerator g;
    auto cube=g.CreateBox(1.5f,1.5f,1.5f,0);
    std::vector<Vertex> verts(cube.Vertices.size());
    for(size_t i=0;i<cube.Vertices.size();++i){
        verts[i].Pos     =cube.Vertices[i].Position;
        verts[i].Normal  =cube.Vertices[i].Normal;
        verts[i].TexCoord=cube.Vertices[i].TexCoord;
    }
    auto idx=cube.GetIndices16();
    UINT vbSz=(UINT)(verts.size()*sizeof(Vertex));
    UINT ibSz=(UINT)(idx.size()*sizeof(uint16_t));
    auto geo=std::make_unique<MeshGeometry>(); geo->Name="cubeGeo";
    ThrowIfFailed(D3DCreateBlob(vbSz,&geo->VertexBufferCPU));
    memcpy(geo->VertexBufferCPU->GetBufferPointer(),verts.data(),vbSz);
    ThrowIfFailed(D3DCreateBlob(ibSz,&geo->IndexBufferCPU));
    memcpy(geo->IndexBufferCPU->GetBufferPointer(),idx.data(),ibSz);
    geo->VertexBufferGPU=d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),mCommandList.Get(),verts.data(),vbSz,geo->VertexBufferUploader);
    geo->IndexBufferGPU =d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),mCommandList.Get(),idx.data(),  ibSz,geo->IndexBufferUploader);
    geo->VertexByteStride=sizeof(Vertex); geo->VertexBufferByteSize=vbSz;
    geo->IndexFormat=DXGI_FORMAT_R16_UINT; geo->IndexBufferByteSize=ibSz;
    SubmeshGeometry sub; sub.IndexCount=(UINT)idx.size();
    geo->DrawArgs["cube"]=sub;
    mGeometries["cubeGeo"]=std::move(geo);
}

void PhongApp::BuildRenderItems()
{
    auto ri=std::make_unique<RenderItem>();
    XMStoreFloat4x4(&ri->World,XMMatrixIdentity());
    ri->ObjCBIndex=0; ri->Geo=mGeometries["cubeGeo"].get();
    ri->IndexCount=ri->Geo->DrawArgs["cube"].IndexCount;
    // Материал куба
    ri->Mat.Ambient ={0.2f,0.2f,0.4f,1};
    ri->Mat.Diffuse ={0.4f,0.6f,1.0f,1};
    ri->Mat.Specular={1,1,1,128};
    mOpaqueRItems.push_back(ri.get());
    mAllRItems.push_back(std::move(ri));
}

void PhongApp::BuildFrameResources()
{
    for(int i=0;i<3;++i)
        mFrameResources.push_back(std::make_unique<FrameResource>(
            md3dDevice.Get(),1,(UINT)mAllRItems.size()));
}

// ════════════════════════════════════════════════════════════════════════════
// Descriptor Heaps: CBV для объектов + проходов, SRV для текстуры
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::BuildDescriptorHeaps()
{
    UINT n=(UINT)mAllRItems.size();
    mPassCbvOffset=n*3;
    mSrvOffset    =(n+1)*3;   // SRV после всех CBV

    D3D12_DESCRIPTOR_HEAP_DESC d={};
    d.NumDescriptors=mSrvOffset+1;  // +1 для текстуры
    d.Type  =D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.Flags =D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&d,IID_PPV_ARGS(&mCbvSrvHeap)));
}

void PhongApp::BuildConstantBufferViews()
{
    UINT objSz =d3dUtil::CalcConstantBufferByteSize(sizeof(CBPerObject));
    UINT passSz=d3dUtil::CalcConstantBufferByteSize(sizeof(CBPerPass));
    UINT n=(UINT)mAllRItems.size();
    for(int f=0;f<3;++f){
        auto cb=mFrameResources[f]->ObjectCB->Resource();
        for(UINT i=0;i<n;++i){
            auto h=CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
                f*n+i,mCbvSrvUavDescriptorSize);
            D3D12_CONSTANT_BUFFER_VIEW_DESC v={
                cb->GetGPUVirtualAddress()+(UINT64)i*objSz, objSz};
            md3dDevice->CreateConstantBufferView(&v,h);
        }
    }
    for(int f=0;f<3;++f){
        auto h=CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
            mPassCbvOffset+f,mCbvSrvUavDescriptorSize);
        auto cb=mFrameResources[f]->PassCB->Resource();
        D3D12_CONSTANT_BUFFER_VIEW_DESC v={cb->GetGPUVirtualAddress(),passSz};
        md3dDevice->CreateConstantBufferView(&v,h);
    }
}

void PhongApp::CreateTextureViews()
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc={};
    srvDesc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format=mTexture->GetDesc().Format;
    srvDesc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels=1;
    auto h=CD3DX12_CPU_HANDLE(mCbvSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        mSrvOffset,mCbvSrvUavDescriptorSize);
    md3dDevice->CreateShaderResourceView(mTexture.Get(),&srvDesc,h);
}

void PhongApp::BuildPSO()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d={};
    d.InputLayout={mInputLayout.data(),(UINT)mInputLayout.size()};
    d.pRootSignature=mRootSignature.Get();
    d.VS={mVsByteCode->GetBufferPointer(),mVsByteCode->GetBufferSize()};
    d.PS={mPsByteCode->GetBufferPointer(),mPsByteCode->GetBufferSize()};
    d.RasterizerState  =CD3DX12_RASTERIZER_DESC_DEFAULT();
    d.BlendState       =CD3DX12_BLEND_DESC_DEFAULT();
    d.DepthStencilState=CD3DX12_DEPTH_STENCIL_DESC_DEFAULT();
    d.SampleMask=UINT_MAX;
    d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets=1; d.RTVFormats[0]=mBackBufferFormat;
    d.SampleDesc.Count=m4xMsaaState?4:1;
    d.SampleDesc.Quality=m4xMsaaState?(m4xMsaaQuality-1):0;
    d.DSVFormat=mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&mPSO)));
}

// ════════════════════════════════════════════════════════════════════════════
// Update
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::OnResize(){D3DApp::OnResize();}

void PhongApp::Update(const GameTimer& gt)
{
    mCurrFrameResourceIndex=(mCurrFrameResourceIndex+1)%3;
    mCurrFrameResource=mFrameResources[mCurrFrameResourceIndex].get();
    if(mCurrFrameResource->Fence!=0&&mFence->GetCompletedValue()<mCurrFrameResource->Fence){
        HANDLE ev=CreateEventEx(nullptr,nullptr,0,EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence,ev));
        WaitForSingleObject(ev,INFINITE); CloseHandle(ev);
    }
    // UV-анимация: смещение по U
    mUVOffset+=gt.DeltaTime()*0.1f;
    if(mUVOffset>1.f) mUVOffset-=1.f;

    UpdateObjectCBs(gt);
    UpdatePassCB(gt);
}

void PhongApp::UpdateObjectCBs(const GameTimer&)
{
    auto* cb=mCurrFrameResource->ObjectCB.get();
    for(auto& e:mAllRItems){
        if(e->NumFramesDirty<=0) continue;
        XMMATRIX w=XMLoadFloat4x4(&e->World);
        XMMATRIX wit=XMMatrixTranspose(XMMatrixInverse(nullptr,w));
        CBPerObject o{};
        XMStoreFloat4x4(&o.World,XMMatrixTranspose(w));
        XMStoreFloat4x4(&o.WorldInvTranspose,XMMatrixTranspose(wit));
        cb->CopyData(e->ObjCBIndex,o); --e->NumFramesDirty;
    }
}

void PhongApp::UpdatePassCB(const GameTimer&)
{
    float x=mRadius*sinf(mPhi)*cosf(mTheta);
    float y=mRadius*cosf(mPhi);
    float z=mRadius*sinf(mPhi)*sinf(mTheta);
    XMVECTOR eye=XMVectorSet(x,y,z,1.f);
    XMStoreFloat4x4(&mMainPassCB.View,XMMatrixTranspose(XMMatrixLookAtLH(eye,XMVectorZero(),XMVectorSet(0,1,0,0))));
    XMStoreFloat4x4(&mMainPassCB.Proj,XMMatrixTranspose(XMLoadFloat4x4(&mProj)));
    XMStoreFloat3(&mMainPassCB.EyePosW,eye);

    mMainPassCB.LightDir     ={ 0.6f,-0.5f, 0.6f };
    mMainPassCB.LightAmbient ={ 0.4f, 0.4f, 0.4f, 1.f };
    mMainPassCB.LightDiffuse ={ 0.8f, 0.8f, 0.8f, 1.f };
    mMainPassCB.LightSpecular={ 1.f,  1.f,  1.f,  1.f };
    mMainPassCB.MatAmbient   ={ 1.f,  1.f,  1.f,  1.f };
    mMainPassCB.MatDiffuse   ={ 1.f,  1.f,  1.f,  1.f };
    mMainPassCB.MatSpecular  ={ 0.6f, 0.6f, 0.6f, 64.f };

    // Тайлинг: текстура повторяется 2x2
    mMainPassCB.TexScale ={ 2.f, 2.f };
    // UV-анимация: плавное смещение по U
    mMainPassCB.TexOffset={ mUVOffset, 0.f };

    mCurrFrameResource->PassCB->CopyData(0,mMainPassCB);
}

// ════════════════════════════════════════════════════════════════════════════
// Draw
// ════════════════════════════════════════════════════════════════════════════
void PhongApp::Draw(const GameTimer&)
{
    auto alloc=mCurrFrameResource->CmdListAlloc;
    ThrowIfFailed(alloc->Reset());
    ThrowIfFailed(mCommandList->Reset(alloc.Get(),mPSO.Get()));
    mCommandList->RSSetViewports(1,&mScreenViewport);
    mCommandList->RSSetScissorRects(1,&mScissorRect);

    auto b1=CD3DX12_RESOURCE_BARRIER_TRANSITION(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    mCommandList->ResourceBarrier(1,&b1);

    const float cc[4]={0.08f,0.08f,0.12f,1.f};
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(),cc,0,nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(),
        D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,1.f,0,0,nullptr);
    mCommandList->OMSetRenderTargets(1,&CurrentBackBufferView(),true,&DepthStencilView());

    ID3D12DescriptorHeap* heaps[]={mCbvSrvHeap.Get()};
    mCommandList->SetDescriptorHeaps(1,heaps);
    mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    // Bind per-pass CBV (slot 1)
    auto passH=CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mPassCbvOffset+mCurrFrameResourceIndex,mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1,passH);

    // Bind texture SRV (slot 2)
    auto srvH=CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
        mSrvOffset,mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(2,srvH);

    DrawRenderItems(mCommandList.Get());

    auto b2=CD3DX12_RESOURCE_BARRIER_TRANSITION(CurrentBackBuffer(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    mCommandList->ResourceBarrier(1,&b2);
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* c[]={mCommandList.Get()};
    mCommandQueue->ExecuteCommandLists(1,c);
    ThrowIfFailed(mSwapChain->Present(0,0));
    mCurrBackBuffer=(mCurrBackBuffer+1)%2;
    mCurrFrameResource->Fence=++mCurrentFence;
    mCommandQueue->Signal(mFence.Get(),mCurrentFence);
}

void PhongApp::DrawRenderItems(ID3D12GraphicsCommandList* cmd)
{
    UINT n=(UINT)mAllRItems.size();
    for(auto* ri:mOpaqueRItems){
        auto vbv=ri->Geo->VertexBufferView();
        auto ibv=ri->Geo->IndexBufferView();
        cmd->IASetVertexBuffers(0,1,&vbv);
        cmd->IASetIndexBuffer(&ibv);
        cmd->IASetPrimitiveTopology(ri->PrimitiveType);
        auto h=CD3DX12_GPU_HANDLE(mCbvSrvHeap->GetGPUDescriptorHandleForHeapStart(),
            mCurrFrameResourceIndex*n+ri->ObjCBIndex,mCbvSrvUavDescriptorSize);
        cmd->SetGraphicsRootDescriptorTable(0,h);
        cmd->DrawIndexedInstanced(ri->IndexCount,1,
            ri->StartIndexLocation,ri->BaseVertexLocation,0);
    }
}

void PhongApp::OnMouseDown(WPARAM,int x,int y){mLastMousePos={x,y};SetCapture(mhMainWnd);}
void PhongApp::OnMouseUp(WPARAM,int,int){ReleaseCapture();}
void PhongApp::OnMouseMove(WPARAM b,int x,int y)
{
    if(b&MK_LBUTTON){
        mTheta+=XMConvertToRadians(0.25f*(x-mLastMousePos.x));
        mPhi=MathHelper::Clamp(mPhi+XMConvertToRadians(0.25f*(y-mLastMousePos.y)),0.1f,XM_PI-0.1f);
    } else if(b&MK_RBUTTON){
        mRadius=MathHelper::Clamp(mRadius+0.005f*((x-mLastMousePos.x)-(y-mLastMousePos.y)),1.f,15.f);
    }
    mLastMousePos={x,y};
}
