#include "ExtensionApi.hpp"
#include "wxl/EventScript.hpp"

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info{
        sizeof(WXL_PluginInfo), WXL_API_VERSION, "wxl-quest-marker", 1, WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;
    wxl_quest_marker::g_api = api;
    wxl::ext::EventScript::Bind(api);

    if (!wxl_quest_marker::ConfigBool("WXL_QUEST_MARKER", true))
    {
        api->Log(WXL_LOG_INFO, "wxl-quest-marker", "extension disabled by configuration");
        return 1;
    }
    if (!wxl_quest_marker::Network())
    {
        api->Log(WXL_LOG_ERROR, "wxl-quest-marker", "required wxl.network v1 is unavailable");
        return 0;
    }
    if (!wxl_quest_marker::FrameScript())
    {
        api->Log(WXL_LOG_ERROR, "wxl-quest-marker", "required wxl.framescript v1 is unavailable");
        return 0;
    }
    return wxl_quest_marker::InstallQuestMarker() ? 1 : 0;
}
