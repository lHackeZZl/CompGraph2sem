#pragma once
// d3dx12.h  — CD3DX12 helpers rewritten as plain inline functions

#include <d3d12.h>
#include <cstring>

inline D3D12_HEAP_PROPERTIES CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p = {};
    p.Type = type; p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1; p.VisibleNodeMask = 1;
    return p;
}

inline D3D12_RESOURCE_DESC CD3DX12_RESOURCE_DESC_BUFFER(UINT64 width, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=width; d.Height=1;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags=flags;
    return d;
}

inline D3D12_RESOURCE_DESC CD3DX12_RESOURCE_DESC_TEX2D(
    DXGI_FORMAT fmt, UINT64 w, UINT h, UINT16 arr=1, UINT16 mip=0,
    UINT sc=1, UINT sq=0, D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=arr; d.MipLevels=mip; d.Format=fmt;
    d.SampleDesc.Count=sc; d.SampleDesc.Quality=sq;
    d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags=flags;
    return d;
}

inline D3D12_RESOURCE_BARRIER CD3DX12_RESOURCE_BARRIER_TRANSITION(
    ID3D12Resource* pRes, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
    UINT sub=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
    D3D12_RESOURCE_BARRIER_FLAGS flags=D3D12_RESOURCE_BARRIER_FLAG_NONE)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Flags=flags;
    b.Transition.pResource=pRes; b.Transition.StateBefore=before;
    b.Transition.StateAfter=after; b.Transition.Subresource=sub;
    return b;
}

inline D3D12_CPU_DESCRIPTOR_HANDLE CD3DX12_CPU_HANDLE(D3D12_CPU_DESCRIPTOR_HANDLE base, INT n, UINT sz)
{ D3D12_CPU_DESCRIPTOR_HANDLE h; h.ptr = base.ptr + SIZE_T(n)*SIZE_T(sz); return h; }

inline D3D12_GPU_DESCRIPTOR_HANDLE CD3DX12_GPU_HANDLE(D3D12_GPU_DESCRIPTOR_HANDLE base, INT n, UINT sz)
{ D3D12_GPU_DESCRIPTOR_HANDLE h; h.ptr = base.ptr + UINT64(n)*UINT64(sz); return h; }

inline D3D12_RASTERIZER_DESC CD3DX12_RASTERIZER_DESC_DEFAULT()
{
    D3D12_RASTERIZER_DESC d = {};
    d.FillMode=D3D12_FILL_MODE_SOLID; d.CullMode=D3D12_CULL_MODE_BACK;
    d.FrontCounterClockwise=FALSE; d.DepthBias=D3D12_DEFAULT_DEPTH_BIAS;
    d.DepthBiasClamp=D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    d.SlopeScaledDepthBias=D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    d.DepthClipEnable=TRUE; d.MultisampleEnable=FALSE;
    d.AntialiasedLineEnable=FALSE; d.ForcedSampleCount=0;
    d.ConservativeRaster=D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    return d;
}

inline D3D12_BLEND_DESC CD3DX12_BLEND_DESC_DEFAULT()
{
    D3D12_BLEND_DESC d = {};
    d.AlphaToCoverageEnable=FALSE; d.IndependentBlendEnable=FALSE;
    const D3D12_RENDER_TARGET_BLEND_DESC rt = {
        FALSE,FALSE,
        D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP, D3D12_COLOR_WRITE_ENABLE_ALL };
    for(auto& r : d.RenderTarget) r = rt;
    return d;
}

inline D3D12_DEPTH_STENCIL_DESC CD3DX12_DEPTH_STENCIL_DESC_DEFAULT()
{
    D3D12_DEPTH_STENCIL_DESC d = {};
    d.DepthEnable=TRUE; d.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc=D3D12_COMPARISON_FUNC_LESS; d.StencilEnable=FALSE;
    d.StencilReadMask=D3D12_DEFAULT_STENCIL_READ_MASK;
    d.StencilWriteMask=D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC op = {
        D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,
        D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS };
    d.FrontFace=d.BackFace=op;
    return d;
}

inline D3D12_CLEAR_VALUE CD3DX12_CLEAR_VALUE_DEPTH(DXGI_FORMAT fmt, FLOAT depth, UINT8 stencil)
{ D3D12_CLEAR_VALUE v={}; v.Format=fmt; v.DepthStencil.Depth=depth; v.DepthStencil.Stencil=stencil; return v; }

inline D3D12_DESCRIPTOR_RANGE CD3DX12_DESCRIPTOR_RANGE(
    D3D12_DESCRIPTOR_RANGE_TYPE type, UINT num, UINT baseReg, UINT space=0,
    UINT offset=D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND)
{
    D3D12_DESCRIPTOR_RANGE r={};
    r.RangeType=type; r.NumDescriptors=num; r.BaseShaderRegister=baseReg;
    r.RegisterSpace=space; r.OffsetInDescriptorsFromTableStart=offset;
    return r;
}

inline D3D12_ROOT_PARAMETER CD3DX12_ROOT_PARAMETER_TABLE(
    UINT numRanges, const D3D12_DESCRIPTOR_RANGE* pRanges,
    D3D12_SHADER_VISIBILITY vis=D3D12_SHADER_VISIBILITY_ALL)
{
    D3D12_ROOT_PARAMETER p={};
    p.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.ShaderVisibility=vis;
    p.DescriptorTable.NumDescriptorRanges=numRanges;
    p.DescriptorTable.pDescriptorRanges=pRanges;
    return p;
}

inline D3D12_ROOT_SIGNATURE_DESC CD3DX12_ROOT_SIGNATURE_DESC(
    UINT numParams, const D3D12_ROOT_PARAMETER* pParams,
    D3D12_ROOT_SIGNATURE_FLAGS flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT)
{
    D3D12_ROOT_SIGNATURE_DESC d={};
    d.NumParameters=numParams; d.pParameters=pParams;
    d.NumStaticSamplers=0; d.pStaticSamplers=nullptr; d.Flags=flags;
    return d;
}
