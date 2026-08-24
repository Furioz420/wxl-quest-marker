// wxl-quest-marker access to the Hub ABI and shared runtime services.
// Copyright (C) 2026 WarcraftXL. GPLv3.

#pragma once

#include "common/ExtensionConfig.hpp"
#include "wxl/FrameScriptApi.h"
#include "wxl/NetworkApi.h"
#include "wxl/PluginApi.h"

namespace wxl_quest_marker
{
    extern const WXL_Api* g_api;
    extern const WXL_NetworkApi* g_network;
    extern const WXL_FrameScriptApi* g_framescript;

    inline const WXL_NetworkApi* Network()
    {
        if (!g_network)
            g_network = static_cast<const WXL_NetworkApi*>(
                g_api->GetInterface("wxl.network", WXL_NETWORK_API_VERSION));
        return g_network;
    }

    inline const WXL_FrameScriptApi* FrameScript()
    {
        if (!g_framescript)
            g_framescript = static_cast<const WXL_FrameScriptApi*>(
                g_api->GetInterface("wxl.framescript", WXL_FRAME_SCRIPT_API_VERSION));
        return g_framescript;
    }

    inline bool ConfigBool(const char* name, bool fallback)
    {
        char value[16] = {};
        return wxl::ext::config::Raw(name, value, sizeof value,
                                     "Extensions\\wxl-quest-marker\\wxl-quest-marker.cfg")
            ? wxl::ext::config::Truthy(value, fallback)
            : fallback;
    }

    bool InstallQuestMarker();
}

#define WLOG_TRACE(...) ::wxl_quest_marker::g_api->Log(WXL_LOG_TRACE, "wxl-quest-marker", __VA_ARGS__)
#define WLOG_DEBUG(...) ::wxl_quest_marker::g_api->Log(WXL_LOG_DEBUG, "wxl-quest-marker", __VA_ARGS__)
#define WLOG_INFO(...)  ::wxl_quest_marker::g_api->Log(WXL_LOG_INFO,  "wxl-quest-marker", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_quest_marker::g_api->Log(WXL_LOG_WARN,  "wxl-quest-marker", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_quest_marker::g_api->Log(WXL_LOG_ERROR, "wxl-quest-marker", __VA_ARGS__)
