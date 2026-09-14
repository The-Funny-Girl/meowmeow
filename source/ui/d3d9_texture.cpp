#include "d3d9_texture.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <vector>

namespace kirkware
{
namespace
{

template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr()
    {
        if (value_ != nullptr)
            value_->Release();
    }

    T* Get() const noexcept { return value_; }
    T** Put() noexcept { return &value_; }
    T* operator->() const noexcept { return value_; }

private:
    T* value_ = nullptr;
};

std::wstring ErrorText(const wchar_t* operation, HRESULT result)
{
    std::wostringstream stream;
    stream << operation << L" failed (HRESULT 0x" << std::hex
           << std::uppercase << std::setw(8) << std::setfill(L'0')
           << static_cast<unsigned long>(result) << L")";
    return stream.str();
}

std::vector<std::uint8_t> DownsampleBox(
    const std::vector<std::uint8_t>& source,
    UINT source_width,
    UINT source_height,
    UINT width,
    UINT height)
{
    std::vector<std::uint8_t> output(
        static_cast<std::size_t>(width) * height * 4u);
    for (UINT y = 0; y < height; ++y)
    {
        const UINT source_y_begin = static_cast<UINT>(
            static_cast<std::uint64_t>(y) * source_height / height);
        const UINT source_y_end = static_cast<UINT>(
            static_cast<std::uint64_t>(y + 1u) * source_height / height);
        for (UINT x = 0; x < width; ++x)
        {
            const UINT source_x_begin = static_cast<UINT>(
                static_cast<std::uint64_t>(x) * source_width / width);
            const UINT source_x_end = static_cast<UINT>(
                static_cast<std::uint64_t>(x + 1u) * source_width / width);
            std::uint32_t totals[4]{};
            std::uint32_t count = 0;
            for (UINT source_y = source_y_begin;
                 source_y < source_y_end; ++source_y)
            {
                for (UINT source_x = source_x_begin;
                     source_x < source_x_end; ++source_x)
                {
                    const std::size_t offset =
                        (static_cast<std::size_t>(source_y) * source_width +
                         source_x) * 4u;
                    for (std::size_t channel = 0; channel < 4u; ++channel)
                        totals[channel] += source[offset + channel];
                    ++count;
                }
            }
            const std::size_t output_offset =
                (static_cast<std::size_t>(y) * width + x) * 4u;
            for (std::size_t channel = 0; channel < 4u; ++channel)
            {
                output[output_offset + channel] =
                    static_cast<std::uint8_t>(
                        (totals[channel] + count / 2u) / count);
            }
        }
    }
    return output;
}

}

D3d9Texture::~D3d9Texture()
{
    Reset();
}

bool D3d9Texture::Load(IDirect3DDevice9* device,
                       HINSTANCE instance,
                       int resource_id,
                       bool mipmapped,
                       std::wstring& error)
{
    Reset();
    if (device == nullptr)
    {
        error = L"Direct3D 9 is unavailable";
        return false;
    }

    HRSRC information = FindResourceW(
        instance, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    HGLOBAL resource = information == nullptr
                           ? nullptr
                           : LoadResource(instance, information);
    const DWORD resource_size = information == nullptr
                                    ? 0
                                    : SizeofResource(instance, information);
    const void* resource_bytes = resource == nullptr
                                     ? nullptr
                                     : LockResource(resource);
    if (resource_bytes == nullptr || resource_size == 0)
    {
        error = L"Embedded PNG resource is unavailable";
        return false;
    }

    using CreateTextureFromMemory = HRESULT(WINAPI*)(
        IDirect3DDevice9*, const void*, UINT, IDirect3DTexture9**);
    HMODULE d3dx9 = GetModuleHandleW(L"d3dx9_43.dll");
    if (d3dx9 == nullptr)
        d3dx9 = LoadLibraryExW(L"d3dx9_43.dll", nullptr,
                               LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (d3dx9 != nullptr)
    {
        const auto create_texture =
            reinterpret_cast<CreateTextureFromMemory>(GetProcAddress(
                d3dx9, "D3DXCreateTextureFromFileInMemory"));
        if (create_texture != nullptr)
        {
            IDirect3DTexture9* texture = nullptr;
            const HRESULT d3dx_result = create_texture(
                device, resource_bytes, resource_size, &texture);
            if (SUCCEEDED(d3dx_result) && texture != nullptr)
            {
                D3DSURFACE_DESC description{};
                const HRESULT description_result =
                    texture->GetLevelDesc(0, &description);
                if (SUCCEEDED(description_result))
                {
                    texture_ = texture;
                    width_ = description.Width;
                    height_ = description.Height;
                    error.clear();
                    return true;
                }
                texture->Release();
            }
        }
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, resource_size);
    if (memory == nullptr)
    {
        error = L"Embedded PNG allocation failed";
        return false;
    }
    void* writable = GlobalLock(memory);
    if (writable == nullptr)
    {
        GlobalFree(memory);
        error = L"Embedded PNG allocation lock failed";
        return false;
    }
    std::memcpy(writable, resource_bytes, resource_size);
    GlobalUnlock(memory);

    ComPtr<IStream> stream;
    HRESULT result = CreateStreamOnHGlobal(memory, TRUE, stream.Put());
    if (FAILED(result))
    {
        GlobalFree(memory);
        error = ErrorText(L"Embedded PNG stream creation", result);
        return false;
    }

    ComPtr<IWICImagingFactory> factory;
    result = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWICImagingFactory,
        reinterpret_cast<void**>(factory.Put()));
    if (FAILED(result))
    {
        error = ErrorText(L"WIC factory creation", result);
        return false;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    result = factory->CreateDecoderFromStream(
        stream.Get(), nullptr,
        WICDecodeMetadataCacheOnLoad, decoder.Put());
    if (FAILED(result))
    {
        error = ErrorText(L"PNG decoder creation", result);
        return false;
    }

    ComPtr<IWICBitmapFrameDecode> frame;
    result = decoder->GetFrame(0, frame.Put());
    if (FAILED(result))
    {
        error = ErrorText(L"PNG frame decode", result);
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    result = frame->GetSize(&width, &height);
    if (FAILED(result) || width == 0 || height == 0 ||
        width > 16384 || height > 16384 ||
        width > (std::numeric_limits<UINT>::max)() / 4u)
    {
        error = FAILED(result)
                    ? ErrorText(L"PNG dimension read", result)
                    : L"PNG dimensions are invalid";
        return false;
    }

    const UINT stride = width * 4u;
    const std::size_t byte_count =
        static_cast<std::size_t>(stride) * height;
    if (byte_count > 512u * 1024u * 1024u ||
        byte_count > (std::numeric_limits<UINT>::max)())
    {
        error = L"PNG dimensions are too large";
        return false;
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(converter.Put());
    if (FAILED(result))
    {
        error = ErrorText(L"WIC converter creation", result);
        return false;
    }
    result = converter->Initialize(
        frame.Get(), GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result))
    {
        error = ErrorText(L"PNG conversion", result);
        return false;
    }

    std::vector<std::uint8_t> pixels(byte_count);
    result = converter->CopyPixels(
        nullptr, stride, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(result))
    {
        error = ErrorText(L"PNG pixel copy", result);
        return false;
    }

    IDirect3DTexture9* texture = nullptr;
    result = device->CreateTexture(
        width, height, mipmapped ? 0u : 1u, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &texture, nullptr);
    if (FAILED(result))
    {
        error = ErrorText(L"Direct3D texture creation", result);
        return false;
    }

    UINT level_width = width;
    UINT level_height = height;
    const DWORD level_count = texture->GetLevelCount();
    for (DWORD level = 0; level < level_count; ++level)
    {
        D3DLOCKED_RECT locked{};
        result = texture->LockRect(level, &locked, nullptr, 0);
        if (FAILED(result))
        {
            texture->Release();
            error = ErrorText(L"Direct3D texture lock", result);
            return false;
        }
        const UINT level_stride = level_width * 4u;
        for (UINT row = 0; row < level_height; ++row)
        {
            std::memcpy(
                static_cast<std::uint8_t*>(locked.pBits) +
                    static_cast<std::size_t>(row) * locked.Pitch,
                pixels.data() + static_cast<std::size_t>(row) * level_stride,
                level_stride);
        }
        result = texture->UnlockRect(level);
        if (FAILED(result))
        {
            texture->Release();
            error = ErrorText(L"Direct3D texture unlock", result);
            return false;
        }
        if (level + 1u < level_count)
        {
            const UINT next_width = level_width > 1u
                                        ? level_width / 2u
                                        : 1u;
            const UINT next_height = level_height > 1u
                                         ? level_height / 2u
                                         : 1u;
            pixels = DownsampleBox(
                pixels, level_width, level_height,
                next_width, next_height);
            level_width = next_width;
            level_height = next_height;
        }
    }

    texture_ = texture;
    width_ = width;
    height_ = height;
    error.clear();
    return true;
}

void D3d9Texture::Reset() noexcept
{
    if (texture_ != nullptr)
    {
        texture_->Release();
        texture_ = nullptr;
    }
    width_ = 0;
    height_ = 0;
}

}
