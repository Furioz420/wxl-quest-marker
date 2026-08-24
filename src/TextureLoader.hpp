#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d9.h>

namespace wxl_quest_marker
{
    struct BlpInfo
    {
        uint32_t width = 0;
        uint32_t height = 0;
        D3DFORMAT format = D3DFMT_UNKNOWN;
        uint32_t mipOffset = 0;
        uint32_t mipSize = 0;
    };

    bool ParseBlpHeader(const uint8_t* data, size_t size, BlpInfo& info);
    void* LoadBlpTexture(IDirect3DDevice9* device, const char* virtualPath);
    void* CreateDebugTexture(IDirect3DDevice9* device);
    void FreeTexture(void* texture);
}
