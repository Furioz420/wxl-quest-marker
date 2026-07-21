#include "TextureLoader.hpp"

#include "core/Logger.hpp"
#include "game/io/Io.hpp"
#include <vector>
#include <cstring>

namespace wxl::scripts::questmarker
{
    namespace
    {
        constexpr uint32_t kBLP2Magic = 0x32504C42;

        uint32_t Rd32(const uint8_t* p)
        {
            return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
        }

        D3DFORMAT BlpAlphaToD3DFormat(uint32_t alphaType)
        {
            switch (alphaType) {
                case 1:  return D3DFMT_DXT3;
                case 7:
                case 8:  return D3DFMT_DXT5;
                default: return D3DFMT_DXT1;
            }
        }
    }

    bool ParseBlpHeader(const uint8_t* data, size_t size, BlpInfo& info)
    {
        if (!data || size < 0x94) return false;
        if (Rd32(data) != kBLP2Magic) return false;

        uint32_t encoding  = data[0x08];
        uint32_t width     = Rd32(data + 0x0C);
        uint32_t height    = Rd32(data + 0x10);
        uint32_t mipOffset = Rd32(data + 0x14);
        uint32_t mipSize   = Rd32(data + 0x54);

        if (encoding == 2)
        {
            uint32_t alphaType = data[0x0A];
            info.format = BlpAlphaToD3DFormat(alphaType);
        }
        else if (encoding == 3)
        {
            info.format = D3DFMT_A8R8G8B8;
        }
        else
        {
            return false;
        }

        info.width     = width;
        info.height    = height;
        info.mipOffset = mipOffset;
        info.mipSize   = mipSize;
        return true;
    }

    void* LoadBlpTexture(IDirect3DDevice9* device, const char* virtualPath)
    {
        if (!device || !virtualPath) return nullptr;

        void* handle = nullptr;
        if (!wxl::game::io::FileOpen(virtualPath, 0, &handle) || !handle)
        {
            WLOG_WARN("texture: file not found '%s'", virtualPath);
            return nullptr;
        }

        uint32_t sizeHigh = 0;
        uint32_t size = wxl::game::io::FileSize(handle, &sizeHigh);
        if (size == 0 || sizeHigh)
        {
            wxl::game::io::FileClose(handle);
            return nullptr;
        }

        std::vector<uint8_t> buf(size);
        uint32_t read = 0;
        wxl::game::io::FileRead(handle, buf.data(), size, &read);
        wxl::game::io::FileClose(handle);

        BlpInfo info;
        if (!ParseBlpHeader(buf.data(), size, info))
        {
            WLOG_WARN("texture: invalid BLP2 '%s'", virtualPath);
            return nullptr;
        }

        if (info.mipOffset + info.mipSize > size)
        {
            WLOG_WARN("texture: mip out of range '%s'", virtualPath);
            return nullptr;
        }

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(device->CreateTexture(info.width, info.height, 1, 0,
                                         info.format, D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
        {
            WLOG_WARN("texture: CreateTexture failed '%s'", virtualPath);
            return nullptr;
        }

        D3DLOCKED_RECT lr;
        if (FAILED(tex->LockRect(0, &lr, nullptr, 0)))
        {
            tex->Release();
            return nullptr;
        }

        const uint8_t* src = buf.data() + info.mipOffset;
        uint8_t* dst = static_cast<uint8_t*>(lr.pBits);
        uint32_t rowSize = (info.width + 3) / 4 * (info.format == D3DFMT_DXT1 ? 8 : 16);
        uint32_t totalRows = (info.height + 3) / 4;

        if (lr.Pitch == static_cast<int>(rowSize))
        {
            std::memcpy(dst, src, info.mipSize);
        }
        else
        {
            for (uint32_t r = 0; r < totalRows; ++r)
                std::memcpy(dst + r * lr.Pitch, src + r * rowSize, rowSize);
        }

        tex->UnlockRect(0);
        WLOG_INFO("texture: loaded '%s' %ux%u", virtualPath, info.width, info.height);
        return tex;
    }

    void* CreateDebugTexture(IDirect3DDevice9* device)
    {
        if (!device) return nullptr;

        IDirect3DTexture9* tex = nullptr;
        if (FAILED(device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
            return nullptr;

        D3DLOCKED_RECT lr;
        if (FAILED(tex->LockRect(0, &lr, nullptr, 0)))
        {
            tex->Release();
            return nullptr;
        }

        auto* pixels = static_cast<uint32_t*>(lr.pBits);
        for (int y = 0; y < 64; ++y)
        {
            for (int x = 0; x < 64; ++x)
            {
                float cx = static_cast<float>(x) - 31.5f;
                float cy = static_cast<float>(y) - 31.5f;
                float d = (cx < 0 ? -cx : cx) + (cy < 0 ? -cy : cy);
                uint32_t c = 0;
                if (d < 28.0f) {
                    float t = 1.0f;
                    if (d > 24.0f) t = (28.0f - d) / 4.0f;
                    uint32_t a = static_cast<uint32_t>(t * 255.0f) & 0xFF;
                    c = (a << 24) | 0x00FFCC00;
                }
                pixels[y * 64 + x] = c;
            }
        }

        tex->UnlockRect(0);
        WLOG_INFO("texture: created debug procedural texture");
        return tex;
    }

    void FreeTexture(void* texture)
    {
        if (texture)
            static_cast<IDirect3DTexture9*>(texture)->Release();
    }
}
