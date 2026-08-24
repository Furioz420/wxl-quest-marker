#include "TextureLoader.hpp"

#include "ExtensionApi.hpp"
#include "game/Io.hpp"

#include <cstring>
#include <vector>

namespace wxl_quest_marker
{
    namespace
    {
        constexpr uint32_t kBlp2Magic = 0x32504C42;

        uint32_t Read32(const uint8_t* p)
        {
            return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24);
        }

        D3DFORMAT BlpAlphaToD3DFormat(uint32_t alphaType)
        {
            switch (alphaType)
            {
                case 1: return D3DFMT_DXT3;
                case 7:
                case 8: return D3DFMT_DXT5;
                default: return D3DFMT_DXT1;
            }
        }
    }

    bool ParseBlpHeader(const uint8_t* data, size_t size, BlpInfo& info)
    {
        if (!data || size < 0x94 || Read32(data) != kBlp2Magic) return false;

        const uint32_t encoding = data[0x08];
        if (encoding == 2)
            info.format = BlpAlphaToD3DFormat(data[0x0A]);
        else if (encoding == 3)
            info.format = D3DFMT_A8R8G8B8;
        else
            return false;

        info.width = Read32(data + 0x0C);
        info.height = Read32(data + 0x10);
        info.mipOffset = Read32(data + 0x14);
        info.mipSize = Read32(data + 0x54);
        return info.width && info.height && info.mipSize;
    }

    void* LoadBlpTexture(IDirect3DDevice9* device, const char* virtualPath)
    {
        if (!device || !virtualPath) return nullptr;

        void* handle = nullptr;
        if (!wxl::game::io::FileOpen(virtualPath, 0, &handle) || !handle)
        {
            WLOG_WARN("navigation texture not found: %s", virtualPath);
            return nullptr;
        }

        uint32_t sizeHigh = 0;
        const uint32_t size = wxl::game::io::FileSize(handle, &sizeHigh);
        if (!size || sizeHigh)
        {
            wxl::game::io::FileClose(handle);
            return nullptr;
        }

        std::vector<uint8_t> buffer(size);
        uint32_t bytesRead = 0;
        wxl::game::io::FileRead(handle, buffer.data(), size, &bytesRead);
        wxl::game::io::FileClose(handle);
        if (bytesRead != size) return nullptr;

        BlpInfo info;
        if (!ParseBlpHeader(buffer.data(), buffer.size(), info) ||
            info.mipOffset > size || info.mipSize > size - info.mipOffset)
        {
            WLOG_WARN("invalid navigation BLP2: %s", virtualPath);
            return nullptr;
        }

        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device->CreateTexture(info.width, info.height, 1, 0, info.format,
                                         D3DPOOL_MANAGED, &texture, nullptr)) || !texture)
            return nullptr;

        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            return nullptr;
        }

        const uint8_t* source = buffer.data() + info.mipOffset;
        auto* destination = static_cast<uint8_t*>(locked.pBits);
        const uint32_t rowSize = (info.width + 3) / 4 *
            (info.format == D3DFMT_DXT1 ? 8u : 16u);
        const uint32_t rows = (info.height + 3) / 4;
        if (locked.Pitch == static_cast<int>(rowSize))
            std::memcpy(destination, source, info.mipSize);
        else
            for (uint32_t row = 0; row < rows; ++row)
                std::memcpy(destination + row * locked.Pitch,
                            source + row * rowSize, rowSize);

        texture->UnlockRect(0);
        WLOG_INFO("loaded navigation atlas %ux%u", info.width, info.height);
        return texture;
    }

    void* CreateDebugTexture(IDirect3DDevice9* device)
    {
        if (!device) return nullptr;
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(device->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8,
                                         D3DPOOL_MANAGED, &texture, nullptr)) || !texture)
            return nullptr;

        D3DLOCKED_RECT locked{};
        if (FAILED(texture->LockRect(0, &locked, nullptr, 0)))
        {
            texture->Release();
            return nullptr;
        }
        for (int y = 0; y < 64; ++y)
        {
            auto* row = reinterpret_cast<uint32_t*>(
                static_cast<uint8_t*>(locked.pBits) + y * locked.Pitch);
            for (int x = 0; x < 64; ++x)
            {
                const float dx = static_cast<float>(x) - 31.5f;
                const float dy = static_cast<float>(y) - 31.5f;
                const float distance = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                uint32_t color = 0;
                if (distance < 28.0f)
                {
                    const float fade = distance > 24.0f ? (28.0f - distance) / 4.0f : 1.0f;
                    color = (static_cast<uint32_t>(fade * 255.0f) << 24) | 0x00FFCC00;
                }
                row[x] = color;
            }
        }
        texture->UnlockRect(0);
        WLOG_WARN("using procedural quest-marker texture fallback");
        return texture;
    }

    void FreeTexture(void* texture)
    {
        if (texture) static_cast<IDirect3DTexture9*>(texture)->Release();
    }
}
