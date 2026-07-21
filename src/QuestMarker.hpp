#pragma once
#include "events/EventScript.hpp"
#include <vector>
#include <cstdint>

namespace wxl::scripts::questmarker
{
    struct MarkerData
    {
        uint32_t questId  = 0;
        bool     active   = false;
        float    x = 0, y = 0, z = 0;
        uint32_t markerType = 0;
    };

    class QuestMarker final : public wxl::events::EventScript
    {
    public:
        QuestMarker();
        ~QuestMarker();

        static int ScriptSetMarker(void* state);
        static int ScriptClearAll(void* state);
        static int ScriptGetDistInfo(void* state);
        static int ScriptSetAlpha(void* state);

        void SetMarker(uint32_t questId, bool active, float x, float y, float z, uint32_t markerType);
        void ClearMarker(uint32_t questId);
        void ClearAllMarkers();

    private:
        void OnWorldRenderEnd(const wxl::events::WorldRenderEndArgs& a);
        void OnDeviceLost(const wxl::events::DeviceResetArgs& a);
        void OnDeviceReset(const wxl::events::DeviceResetArgs& a);

        void InitDevice(void* device);
        void ReleaseDevice();

        std::vector<MarkerData> m_markers;
        float    m_ndcX = 0, m_ndcY = 0;
        float    m_dist = 0;
        bool     m_markerValid = false;
        bool     m_deviceReady = false;
        void*    m_d3dTexture = nullptr;
        void*    m_vb = nullptr;
        int      m_vbSize = 0;
        uint32_t m_alpha = 160;
    };
}
