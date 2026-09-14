#pragma once

#include <d3d9.h>
#include <windows.h>

#include <string>

namespace kirkware
{

class D3d9Texture
{
public:
    D3d9Texture() = default;
    D3d9Texture(const D3d9Texture&) = delete;
    D3d9Texture& operator=(const D3d9Texture&) = delete;
    ~D3d9Texture();

    bool Load(IDirect3DDevice9* device,
              HINSTANCE instance,
              int resource_id,
              bool mipmapped,
              std::wstring& error);
    void Reset() noexcept;

    IDirect3DTexture9* Get() const noexcept { return texture_; }
    unsigned Width() const noexcept { return width_; }
    unsigned Height() const noexcept { return height_; }

private:
    IDirect3DTexture9* texture_ = nullptr;
    unsigned width_ = 0;
    unsigned height_ = 0;
};

}
