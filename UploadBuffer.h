#pragma once
// UploadBuffer.h  — GPU upload-heap constant/structured buffer (Frank Luna style)

#include <d3d12.h>
#include <wrl/client.h>
#include <cassert>

// Helper: round up to 256-byte alignment (D3D12 CB requirement)
inline UINT CalcCBByteSize(UINT byteSize)
{
    return (byteSize + 255) & ~255;
}

template<typename T>
class UploadBuffer
{
public:
    UploadBuffer(ID3D12Device* device, UINT elementCount, bool isConstantBuffer)
        : mIsConstantBuffer(isConstantBuffer)
    {
        mElementByteSize = isConstantBuffer
            ? CalcCBByteSize(sizeof(T))
            : sizeof(T);

        D3D12_HEAP_PROPERTIES heapProp{};
        heapProp.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC resDesc{};
        resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        resDesc.Width            = (UINT64)mElementByteSize * elementCount;
        resDesc.Height           = 1;
        resDesc.DepthOrArraySize = 1;
        resDesc.MipLevels        = 1;
        resDesc.SampleDesc.Count = 1;
        resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        device->CreateCommittedResource(
            &heapProp, D3D12_HEAP_FLAG_NONE,
            &resDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&mUploadBuffer));

        mUploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mMappedData));
        // Buffer stays mapped until destroyed
    }

    UploadBuffer(const UploadBuffer&) = delete;
    UploadBuffer& operator=(const UploadBuffer&) = delete;

    ~UploadBuffer()
    {
        if (mUploadBuffer) mUploadBuffer->Unmap(0, nullptr);
        mMappedData = nullptr;
    }

    ID3D12Resource* Resource() const { return mUploadBuffer.Get(); }

    void CopyData(int elementIndex, const T& data)
    {
        memcpy(&mMappedData[(size_t)elementIndex * mElementByteSize], &data, sizeof(T));
    }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> mUploadBuffer;
    BYTE*  mMappedData       = nullptr;
    UINT   mElementByteSize  = 0;
    bool   mIsConstantBuffer = false;
};
