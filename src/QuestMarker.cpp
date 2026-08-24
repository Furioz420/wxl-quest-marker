#include "QuestMarker.hpp"
#include "QuestHighlight.hpp"
#include "ExtensionApi.hpp"
#include "TextureLoader.hpp"

#include "game/Gx.hpp"
#include "game/Camera.hpp"
#include "game/Script.hpp"
#include "game/World.hpp"
#include "offsets/game/World.hpp"

#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>

#define D3DFVF_MARKERVERTS (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

namespace wxl_quest_marker
{
    namespace gx    = wxl::game::gx;
    namespace cam   = wxl::game::camera;
    namespace script = wxl::game::script;
    namespace world = wxl::game::world;
    namespace worldoff = wxl::offsets::game::world;

    namespace
    {
        constexpr uint16_t kCmsgQuestMarkerRequest = 0x051F;
        constexpr uint16_t kSmsgQuestMarkerUpdate = 0x0520;
        constexpr uint16_t kSmsgQuestKillEntries = 0x0536;
        constexpr uint16_t kSmsgQuestCorpsePosition = 0x0537;

        bool ProjectToUi(const float position[3], float& screenX, float& screenY,
                         bool& visible, float& ddcWidth, float& ddcHeight)
        {
            void* worldFrame = *reinterpret_cast<void**>(worldoff::kWorldFrame);
            if (!worldFrame) return false;
            float projected[3]{};
            uint32_t clipFlags = 0;
            const int onScreen = wxl::game::Native<worldoff::GetScreenCoordinatesFn>(
                worldoff::kGetScreenCoordinates)(
                    worldFrame, nullptr, position, projected, &clipFlags);
            screenX = projected[0];
            screenY = projected[1];
            ddcWidth = *reinterpret_cast<const float*>(worldoff::kDdcWidth);
            ddcHeight = *reinterpret_cast<const float*>(worldoff::kDdcHeight);
            if (!(ddcWidth > 0.0f) || !(ddcHeight > 0.0f)) return false;
            visible = onScreen != 0;
            return true;
        }

        constexpr float kMarkerPixelSize = 16.0f;
        constexpr float kMaxDistSq = 100000.0f * 100000.0f;

        QuestMarker* g_instance = nullptr;

        struct MarkerPacket
        {
            uint32_t questId = 0;
            uint32_t active = 0;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
            uint32_t markerType = 0;
        };

        struct KillEntriesPacket
        {
            uint32_t count = 0;
            uint32_t entries[4] = {};
        };

        struct CorpsePacket
        {
            uint32_t active = 0;
            double x = 0.0;
            double y = 0.0;
            double z = 0.0;
        };

        std::mutex g_packetMutex;
        std::deque<MarkerPacket> g_markerPackets;
        std::deque<KillEntriesPacket> g_killPackets;
        std::deque<CorpsePacket> g_corpsePackets;

        template <typename T>
        bool ReadPacket(std::span<const uint8_t> payload, size_t& cursor, T& value)
        {
            if (cursor > payload.size() || sizeof(T) > payload.size() - cursor)
                return false;
            std::memcpy(&value, payload.data() + cursor, sizeof(T));
            cursor += sizeof(T);
            return true;
        }

        void NotifyLua()
        {
            FrameScript()->Execute(
                "if wxlwow and wxlwow.quest_tracker and "
                "wxlwow.quest_tracker._NativeChanged then "
                "wxlwow.quest_tracker._NativeChanged() end",
                "quest-tracker-packet");
        }

        void OnMarkerPacket(const uint8_t* bytes, uint32_t size, void*)
        {
            std::span<const uint8_t> payload(bytes, size);
            MarkerPacket packet;
            size_t cursor = 0;
            if (size != 36 || !ReadPacket(payload, cursor, packet.questId) ||
                !ReadPacket(payload, cursor, packet.active) ||
                !ReadPacket(payload, cursor, packet.x) ||
                !ReadPacket(payload, cursor, packet.y) ||
                !ReadPacket(payload, cursor, packet.z) ||
                !ReadPacket(payload, cursor, packet.markerType) || cursor != payload.size() ||
                packet.active > 1 || packet.markerType > 2)
            {
                WLOG_WARN("rejected malformed quest-marker packet (%u bytes)", size);
                return;
            }

            {
                std::lock_guard lock(g_packetMutex);
                if (g_markerPackets.size() >= 256) g_markerPackets.pop_front();
                g_markerPackets.push_back(packet);
            }
            NotifyLua();
        }

        void OnKillEntriesPacket(const uint8_t* bytes, uint32_t size, void*)
        {
            std::span<const uint8_t> payload(bytes, size);
            KillEntriesPacket packet;
            size_t cursor = 0;
            if (!ReadPacket(payload, cursor, packet.count) || packet.count > 4 ||
                size != 4 + packet.count * sizeof(uint32_t))
            {
                WLOG_WARN("rejected malformed quest kill-entry packet (%u bytes)", size);
                return;
            }
            for (uint32_t i = 0; i < packet.count; ++i)
                if (!ReadPacket(payload, cursor, packet.entries[i])) return;

            SetQuestHighlightEntries(packet.entries, packet.count);
            {
                std::lock_guard lock(g_packetMutex);
                if (g_killPackets.size() >= 32) g_killPackets.pop_front();
                g_killPackets.push_back(packet);
            }
            NotifyLua();
        }

        void OnCorpsePacket(const uint8_t* bytes, uint32_t size, void*)
        {
            std::span<const uint8_t> payload(bytes, size);
            CorpsePacket packet;
            size_t cursor = 0;
            if (!ReadPacket(payload, cursor, packet.active) || packet.active > 1 ||
                (packet.active == 0 && size != 4) || (packet.active == 1 && size != 28))
            {
                WLOG_WARN("rejected malformed corpse-marker packet (%u bytes)", size);
                return;
            }
            if (packet.active && (!ReadPacket(payload, cursor, packet.x) ||
                !ReadPacket(payload, cursor, packet.y) || !ReadPacket(payload, cursor, packet.z)))
                return;

            {
                std::lock_guard lock(g_packetMutex);
                if (g_corpsePackets.size() >= 16) g_corpsePackets.pop_front();
                g_corpsePackets.push_back(packet);
            }
            NotifyLua();
        }

        int __cdecl ScriptRequest(void* state)
        {
            uint16_t request = static_cast<uint16_t>(script::ToNumber(state, 1));
            uint32_t questId = static_cast<uint32_t>(script::ToNumber(state, 2));
            uint8_t payload[6] = {};
            std::memcpy(payload, &request, sizeof request);
            uint32_t size = sizeof request;
            if (request == 101 || request == 103)
            {
                std::memcpy(payload + sizeof request, &questId, sizeof questId);
                size += sizeof questId;
            }
            script::PushBoolean(state,
                (request == 101 || request == 102 || request == 103) &&
                Network()->Send(kCmsgQuestMarkerRequest, payload, size) != 0);
            return 1;
        }

        int __cdecl ScriptPopMarker(void* state)
        {
            MarkerPacket packet;
            {
                std::lock_guard lock(g_packetMutex);
                if (g_markerPackets.empty()) return 0;
                packet = g_markerPackets.front();
                g_markerPackets.pop_front();
            }
            script::PushNumber(state, packet.questId);
            script::PushNumber(state, packet.active);
            script::PushNumber(state, packet.x);
            script::PushNumber(state, packet.y);
            script::PushNumber(state, packet.z);
            script::PushNumber(state, packet.markerType);
            return 6;
        }

        int __cdecl ScriptPopKillEntries(void* state)
        {
            KillEntriesPacket packet;
            {
                std::lock_guard lock(g_packetMutex);
                if (g_killPackets.empty()) return 0;
                packet = g_killPackets.front();
                g_killPackets.pop_front();
            }
            script::PushNumber(state, packet.count);
            for (uint32_t i = 0; i < packet.count; ++i)
                script::PushNumber(state, packet.entries[i]);
            return 1 + static_cast<int>(packet.count);
        }

        int __cdecl ScriptPopCorpse(void* state)
        {
            CorpsePacket packet;
            {
                std::lock_guard lock(g_packetMutex);
                if (g_corpsePackets.empty()) return 0;
                packet = g_corpsePackets.front();
                g_corpsePackets.pop_front();
            }
            script::PushNumber(state, packet.active);
            if (!packet.active) return 1;
            script::PushNumber(state, packet.x);
            script::PushNumber(state, packet.y);
            script::PushNumber(state, packet.z);
            return 4;
        }

        struct MarkerVert
        {
            float x, y, z;
            uint32_t color;
            float tu, tv;
        };


        class SavedMarkerStates
        {
        public:
            DWORD zEnable = TRUE, zWrite = FALSE, alphaBlend = FALSE;
            DWORD srcBlend = 0, dstBlend = 0, alphaTest = FALSE;
            DWORD cull = D3DCULL_CCW, lighting = TRUE;
            DWORD colorWrite = 0x0F, fog = FALSE, stencil = FALSE;
            IDirect3DVertexShader9* vs = nullptr;
            IDirect3DPixelShader9* ps = nullptr;
            IDirect3DDevice9* dev = nullptr;
            bool captured = false;
            // Transforms
            D3DMATRIX matWorld, matView, matProj;
            // Texture stage states for all 8 stages
            DWORD tssColorOp[8], tssColorArg1[8], tssColorArg2[8];
            DWORD tssAlphaOp[8], tssAlphaArg1[8], tssAlphaArg2[8];
            // Sampler states for stage 0
            DWORD sampMipFilter, sampMinFilter, sampMagFilter;
            // ZFUNC, texture 0
            DWORD zFunc;
            IDirect3DBaseTexture9* tex0 = nullptr;
            // Stream source, FVF, alpha func/ref
            IDirect3DVertexBuffer9* streamVB = nullptr;
            UINT streamOffset = 0;
            UINT streamStride = 0;
            DWORD fvf = 0;
            DWORD alphaFunc = 0;
            DWORD alphaRef = 0;
            DWORD blendOp = 0;
            DWORD srgbWrite = 0;

            void Capture(IDirect3DDevice9* d)
            {
                dev = d;
                d->GetRenderState(D3DRS_ZENABLE, &zEnable);
                d->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
                d->GetRenderState(D3DRS_ALPHABLENDENABLE, &alphaBlend);
                d->GetRenderState(D3DRS_SRCBLEND, &srcBlend);
                d->GetRenderState(D3DRS_DESTBLEND, &dstBlend);
                d->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
                d->GetRenderState(D3DRS_CULLMODE, &cull);
                d->GetRenderState(D3DRS_LIGHTING, &lighting);
                d->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
                d->GetRenderState(D3DRS_FOGENABLE, &fog);
                d->GetRenderState(D3DRS_STENCILENABLE, &stencil);
                // D3D9 Get* interface methods already return an AddRef'd pointer. Adding another
                // reference here leaked the currently bound shaders once per rendered marker frame.
                // More importantly, doing the same for a bound DEFAULT-pool texture below kept that
                // resource alive after OnDeviceLost, so Reset could never leave
                // D3DERR_DEVICENOTRESET after minimizing/restoring the client.
                d->GetVertexShader(&vs);
                d->GetPixelShader(&ps);
                // Transforms
                d->GetTransform(D3DTS_WORLD, &matWorld);
                d->GetTransform(D3DTS_VIEW, &matView);
                d->GetTransform(D3DTS_PROJECTION, &matProj);
                // Texture stage states for all 8 stages
                for (DWORD s = 0; s < 8; ++s) {
                    d->GetTextureStageState(s, D3DTSS_COLOROP,   &tssColorOp[s]);
                    d->GetTextureStageState(s, D3DTSS_COLORARG1, &tssColorArg1[s]);
                    d->GetTextureStageState(s, D3DTSS_COLORARG2, &tssColorArg2[s]);
                    d->GetTextureStageState(s, D3DTSS_ALPHAOP,   &tssAlphaOp[s]);
                    d->GetTextureStageState(s, D3DTSS_ALPHAARG1, &tssAlphaArg1[s]);
                    d->GetTextureStageState(s, D3DTSS_ALPHAARG2, &tssAlphaArg2[s]);
                }
                // Sampler states for stage 0
                d->GetSamplerState(0, D3DSAMP_MIPFILTER, &sampMipFilter);
                d->GetSamplerState(0, D3DSAMP_MINFILTER, &sampMinFilter);
                d->GetSamplerState(0, D3DSAMP_MAGFILTER, &sampMagFilter);
                // ZFUNC, texture 0
                d->GetRenderState(D3DRS_ZFUNC, &zFunc);
                d->GetTexture(0, &tex0);
                // Stream source, FVF, alpha func/ref
                d->GetStreamSource(0, &streamVB, &streamOffset, &streamStride);
                d->GetFVF(&fvf);
                d->GetRenderState(D3DRS_ALPHAFUNC, &alphaFunc);
                d->GetRenderState(D3DRS_ALPHAREF, &alphaRef);
                d->GetRenderState(D3DRS_BLENDOP, &blendOp);
                { union { DWORD d; float f; } u; d->GetRenderState(D3DRS_SRGBWRITEENABLE, &u.d); srgbWrite = u.d; }
                captured = true;
            }

            void Restore()
            {
                if (!dev || !captured) return;
                captured = false;
                dev->SetRenderState(D3DRS_ZENABLE, zEnable);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);
                dev->SetRenderState(D3DRS_ALPHABLENDENABLE, alphaBlend);
                dev->SetRenderState(D3DRS_SRCBLEND, srcBlend);
                dev->SetRenderState(D3DRS_DESTBLEND, dstBlend);
                dev->SetRenderState(D3DRS_ALPHATESTENABLE, alphaTest);
                dev->SetRenderState(D3DRS_CULLMODE, cull);
                dev->SetRenderState(D3DRS_LIGHTING, lighting);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorWrite);
                dev->SetRenderState(D3DRS_FOGENABLE, fog);
                dev->SetRenderState(D3DRS_STENCILENABLE, stencil);
                dev->SetVertexShader(vs);
                dev->SetPixelShader(ps);
                if (vs) vs->Release();
                if (ps) ps->Release();
                vs = nullptr; ps = nullptr;
                // Transforms
                dev->SetTransform(D3DTS_WORLD, &matWorld);
                dev->SetTransform(D3DTS_VIEW, &matView);
                dev->SetTransform(D3DTS_PROJECTION, &matProj);
                // Texture stage states
                for (DWORD s = 0; s < 8; ++s) {
                    dev->SetTextureStageState(s, D3DTSS_COLOROP,   tssColorOp[s]);
                    dev->SetTextureStageState(s, D3DTSS_COLORARG1, tssColorArg1[s]);
                    dev->SetTextureStageState(s, D3DTSS_COLORARG2, tssColorArg2[s]);
                    dev->SetTextureStageState(s, D3DTSS_ALPHAOP,   tssAlphaOp[s]);
                    dev->SetTextureStageState(s, D3DTSS_ALPHAARG1, tssAlphaArg1[s]);
                    dev->SetTextureStageState(s, D3DTSS_ALPHAARG2, tssAlphaArg2[s]);
                }
                // Sampler states
                dev->SetSamplerState(0, D3DSAMP_MIPFILTER, sampMipFilter);
                dev->SetSamplerState(0, D3DSAMP_MINFILTER, sampMinFilter);
                dev->SetSamplerState(0, D3DSAMP_MAGFILTER, sampMagFilter);
                // ZFUNC, texture 0
                dev->SetRenderState(D3DRS_ZFUNC, zFunc);
                dev->SetTexture(0, tex0);
                if (tex0) { tex0->Release(); tex0 = nullptr; }
                // Stream source, FVF, alpha func/ref
                dev->SetStreamSource(0, streamVB, streamOffset, streamStride);
                if (streamVB) streamVB->Release();
                dev->SetFVF(fvf);
                dev->SetRenderState(D3DRS_ALPHAFUNC, alphaFunc);
                dev->SetRenderState(D3DRS_ALPHAREF, alphaRef);
                dev->SetRenderState(D3DRS_BLENDOP, blendOp);
                dev->SetRenderState(D3DRS_SRGBWRITEENABLE, srgbWrite);
            }

            ~SavedMarkerStates() { Restore(); }
        };

    }

    int QuestMarker::ScriptSetMarker(void* state)
    {
        if (!g_instance) return 0;
        const uint32_t questId    = static_cast<uint32_t>(script::ToNumber(state, 1));
        const bool     active     = script::ToNumber(state, 2) != 0.0;
        const double   x          = script::ToNumber(state, 3);
        const double   y          = script::ToNumber(state, 4);
        const double   z          = script::ToNumber(state, 5);
        g_instance->SetMarker(questId, active, static_cast<float>(x),
                             static_cast<float>(y), static_cast<float>(z));
        return 0;
    }

    int QuestMarker::ScriptSetTrackerDiamond(void* state)
    {
        if (!g_instance) return 0;
        g_instance->m_trackerDiamondActive = script::ToNumber(state, 1) != 0.0;
        g_instance->m_trackerNdcX     = static_cast<float>(script::ToNumber(state, 2));
        g_instance->m_trackerNdcY     = static_cast<float>(script::ToNumber(state, 3));
        g_instance->m_trackerNdcHalfW = static_cast<float>(script::ToNumber(state, 4));
        g_instance->m_trackerNdcHalfH = static_cast<float>(script::ToNumber(state, 5));
        return 0;
    }

    int QuestMarker::ScriptClearAll(void* state)
    {
        if (g_instance) g_instance->ClearAllMarkers();
        return 0;
    }

    int QuestMarker::ScriptGetDistInfo(void* state)
    {
        if (!g_instance || !g_instance->m_markerValid) { script::PushNil(state); script::PushNil(state); script::PushNil(state); return 3; }
        script::PushNumber(state, g_instance->m_ndcX);
        script::PushNumber(state, g_instance->m_ndcY);
        script::PushNumber(state, g_instance->m_dist);
        return 3;
    }

    int QuestMarker::ScriptSetAlpha(void* state)
    {
        auto alpha = static_cast<uint32_t>(script::ToNumber(state, 1));
        if (alpha > 255) alpha = 255;
        if (g_instance) g_instance->m_alpha = alpha;
        return 0;
    }

    int QuestMarker::ScriptSetCorpseMarker(void* state)
    {
        const double x = script::ToNumber(state, 1);
        const double y = script::ToNumber(state, 2);
        const double z = script::ToNumber(state, 3);
        if (g_instance) g_instance->SetCorpseMarker(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
        return 0;
    }

    int QuestMarker::ScriptClearCorpseMarker(void* state)
    {
        if (g_instance) g_instance->ClearCorpseMarker();
        return 0;
    }

    int QuestMarker::ScriptIsCorpseActive(void* state)
    {
        if (!g_instance || !g_instance->m_corpseActive) { script::PushNil(state); return 1; }
        script::PushNumber(state, 1.0);
        return 1;
    }

    int QuestMarker::ScriptSetDeadState(void* state)
    {
        if (g_instance) g_instance->m_deadState = script::ToNumber(state, 1) != 0.0;
        return 0;
    }

    int QuestMarker::ScriptGetPlayerPos(void* state)
    {
        unsigned long long guid = world::ActivePlayerGuid();
        if (!guid) { script::PushNil(state); return 1; }
        void* playerObj = world::ResolveObject(guid, world::kTypeMaskPlayer);
        if (!playerObj) { script::PushNil(state); return 1; }

        float pos[3];
        world::UnitPosition(playerObj, pos);
        script::PushNumber(state, pos[0]);
        script::PushNumber(state, pos[1]);
        script::PushNumber(state, pos[2]);
        return 3;
    }

    int QuestMarker::ScriptGetCorpseDistInfo(void* state)
    {
        if (!g_instance || !g_instance->m_corpseNdcValid) { script::PushNil(state); script::PushNil(state); script::PushNil(state); return 3; }
        script::PushNumber(state, g_instance->m_corpseNdcX);
        script::PushNumber(state, g_instance->m_corpseNdcY);
        script::PushNumber(state, g_instance->m_corpseDist);
        return 3;
    }

    // Arg layout: enabled, showMarker, alpha, worldMarkerScale,
    // offscreenScale, offscreenOffsetX, offscreenOffsetY, showOffscreen
    int QuestMarker::ScriptReloadSettings(void* state)
    {
        if (!g_instance) return 0;
        auto& s = g_instance->m_settings;
        s.enabled    = script::ToNumber(state, 1) != 0.0;
        s.showMarker = script::ToNumber(state, 2) != 0.0;
        s.alpha      = static_cast<uint8_t>(script::ToNumber(state, 3));
        s.worldMarkerScale = std::clamp(
            static_cast<float>(script::ToNumber(state, 4)), 0.5f, 2.0f);
        s.offscreenScale = std::clamp(
            static_cast<float>(script::ToNumber(state, 5)), 0.5f, 2.0f);
        s.offscreenOffsetX = std::clamp(
            static_cast<float>(script::ToNumber(state, 6)), -500.0f, 500.0f);
        s.offscreenOffsetY = std::clamp(
            static_cast<float>(script::ToNumber(state, 7)), -350.0f, 350.0f);
        s.showOffscreen = script::ToNumber(state, 8) != 0.0;
        g_instance->m_alpha = s.alpha;
        return 0;
    }

    #if 0
    static void InstallLegacyEmbeddedModule()
    {
        wxl::runtime::lua::RegisterFunction("SetQuestMarker", &QuestMarker::ScriptSetMarker);
        wxl::runtime::lua::RegisterFunction("ClearAllQuestMarkers", &QuestMarker::ScriptClearAll);
        wxl::runtime::lua::RegisterFunction("GetDistInfo", &QuestMarker::ScriptGetDistInfo);
        wxl::runtime::lua::RegisterFunction("SetMarkerAlpha", &QuestMarker::ScriptSetAlpha);
        wxl::runtime::lua::RegisterFunction("QuestTrackerReloadSettings", &QuestMarker::ScriptReloadSettings);
        wxl::runtime::lua::RegisterFunction("SetCorpseMarker", &QuestMarker::ScriptSetCorpseMarker);
        wxl::runtime::lua::RegisterFunction("ClearCorpseMarker", &QuestMarker::ScriptClearCorpseMarker);
        wxl::runtime::lua::RegisterFunction("IsCorpseActive", &QuestMarker::ScriptIsCorpseActive);
        wxl::runtime::lua::RegisterFunction("GetCorpseDistInfo", &QuestMarker::ScriptGetCorpseDistInfo);
        wxl::runtime::lua::RegisterFunction("SetDeadState", &QuestMarker::ScriptSetDeadState);
        wxl::runtime::lua::RegisterFunction("GetPlayerPosition", &QuestMarker::ScriptGetPlayerPos);
        wxl::runtime::lua::RegisterFunction("SetTrackerDiamond", &QuestMarker::ScriptSetTrackerDiamond);

        wxl::runtime::lua::RegisterCVar("wxTrackerEnabled", "1");
        wxl::runtime::lua::RegisterCVar("wxTrackerShowMarker", "1");
        wxl::runtime::lua::RegisterCVar("wxTrackerAlpha", "165");
        wxl::runtime::lua::RegisterScript("wxl-quest-marker", R"lua()lua" R"lua(
            local cache = {}
            local currentQuest = 0
            local lastKillQuest = 0  -- last quest we requested kill entries for (dedupe)
            local distFrame = nil
            local distText = nil
            wxqmEnabled = true
            wxqmMarkerAlpha = 165

            local function formatDist(d)
                if d >= 1000 then return string.format("%.1f km", d / 1000)
                else return string.format("%.0f yd", d) end
            end

            local function SendKillEntriesRequest(questId)
                if not questId then return end
                local pkt = CreateCustomPacket(100, 6)
                pkt:WriteUInt16(103)
                pkt:WriteUInt32(questId)
                pkt:Send()
            end

            -- Quest-info stream (kill/interaction creature entries) is INDEPENDENT
            -- of the marker: the server computes it from quest_template regardless
            -- of zone eligibility, so it must follow the SELECTED quest, not the
            -- marker cache. Request it whenever the selected quest changes. Deduped
            -- so the per-frame SyncToSelection never spams opcode 103.
            -- The L/M selection registers can PING-PONG across two quests (the
            -- WatchFrame re-selects the map's saved quest while the quest log
            -- holds a different selection), which defeats a pure change latch and
            -- floods opcode 103/101 at per-frame rate -- the core's anti-DoS then
            -- kicks the player (DosProtection::EvaluateOpcode AntiDOS). A time
            -- cooldown caps the rate instead: at most one kill-entry request per
            -- 2s per session, even while the registers oscillate.
            local lastKillTime = 0
            local function KillRequestAllowed(questId)
                if not (questId and questId > 0) then return false end
                local now = GetTime()
                if questId == lastKillQuest or now - lastKillTime < 2 then return false end
                lastKillQuest = questId
                lastKillTime = now
                return true
            end
            local function RequestKillEntriesIfChanged(questId)
                if KillRequestAllowed(questId) then
                    SendKillEntriesRequest(questId)
                end
            end

            -- Clearing quest info must reach every consumer (kill-highlight and
            -- selcircle-poc share the same opcode-200 stream). Resetting
            -- lastKillQuest forces the next selection to re-request after a clear
            -- (zone change / login), so the highlight can never stay empty.
            local function ClearKillEntries()
                lastKillQuest = 0
                lastKillTime = 0
                if ClearKillObjectiveEntries then ClearKillObjectiveEntries() end
                if ClearSelCircleQuestEntries then ClearSelCircleQuestEntries() end
            end

            local function SwitchTo(questId, fromServer)
                local c = cache[questId]
                if questId and questId > 0 then
                    if c then
                        -- Kill entries follow the SELECTED quest (not the marker
                        -- cache): request them on every quest switch (M tracker /
                        -- dropdown / login auto-track / zone change).
                        RequestKillEntriesIfChanged(questId)
                    elseif not fromServer then
                        -- No cached marker (e.g. packet was swallowed by lockout on login).
                        -- 101 resends BOTH marker and kill entries, so the dedupe
                        -- must consider it already requested. Same time-cooldown
                        -- latch as the 103 path (see KillRequestAllowed).
                        if KillRequestAllowed(questId) then
                            local pkt = CreateCustomPacket(100, 6)
                            pkt:WriteUInt16(101)
                            pkt:WriteUInt32(questId)
                            pkt:Send()
                        end
                    else
                        -- Selected/auto-tracked quest with NO marker data in this
                        -- zone: still request its kill entries — the quest-mob
                        -- highlight follows the selection even when the marker is
                        -- hidden (the server computes entries zone-independently).
                        RequestKillEntriesIfChanged(questId)
                    end
                end
                if c then
                    if currentQuest ~= 0 and currentQuest ~= questId then
                        -- Deactivate only the previous quest's marker instead of clearing all.
                        -- This prevents visible flicker during login batch sends.
                        SetQuestMarker(currentQuest, 0, 0, 0, 0)
                    end
                    SetMarkerAlpha(wxqmMarkerAlpha)
                    SetQuestMarker(questId, 1, c[1], c[2], c[3])
                    currentQuest = questId
                end
            end

            -- Login batch: during initial marker load, the server sends markers
            -- for ALL active quests in quick succession. SwitchTo on each individual
            -- packet makes the marker visibly jump through every quest's coordinates.
            -- Instead: show the FIRST marker immediately (instant), but suppress ALL
            -- subsequent switches during the batch. The batch exits after 500ms,
            -- at which point normal per-packet switching resumes.
            local loginBatch = true
            local batchTimer = nil  -- created lazily when first marker arrives
            local showTimer = nil   -- zone-change refresh finalizer (cancelled by batch sentinel)
            local zoneRefreshing = false  -- suppress auto-switch during zone-change refresh
            local initialized = false  -- one-time UI setup guard (PEW fires on every login)

            -- First quest shown in the world map quest list when M is pressed.
            -- The map builds its list from the quest-POI system for the current
            -- zone (QuestMapUpdateAllQuests + QuestPOIGetQuestIDByVisibleIndex),
            -- NOT the quest log order — so replicate exactly that to mirror the
            -- M tracker. QuestMapUpdateAllQuests is refreshed at most every 0.5s
            -- to keep the per-frame SyncToSelection cheap.
            local poiRefreshTime = 0
            local function FirstMapListQuestId()
                if not QuestMapUpdateAllQuests or not QuestPOIGetQuestIDByVisibleIndex then
                    return 0
                end
                local now = GetTime()
                if now - poiRefreshTime > 0.5 then
                    poiRefreshTime = now
                    QuestMapUpdateAllQuests()
                end
                local qid, qIndex = QuestPOIGetQuestIDByVisibleIndex(1)
                if qIndex and qIndex > 0 and qid and qid > 0 then
                    return qid
                end
                return 0
            end


)lua"
        R"lua(
            -- ARCHITECTURE (authoritative): the marker has ZERO intelligence.
            --  1. Quest Log (L): Blizzard's GLOBAL quest selection. Any quest can
            --     be selected; selecting a quest does not guarantee a marker.
            --  2. Zone Eligibility (SERVER): the server decides which quests are
            --     currently eligible (Retail-like, via quest_poi WorldMapAreaId
            --     belonging to the player's current zone) and streams marker data
            --     for eligible quests ONLY. The server never picks the quest to
            --     display - it only fills the cache below.
            --  3. Marker Renderer (CLIENT): renders the currently selected quest
            --     ONLY if marker data exists in the cache (i.e. it is currently
            --     eligible). Otherwise the marker is hidden.
            -- The marker NEVER chooses, seeds, promotes or substitutes a quest.
            -- Blizzard keeps the selection in TWO registers:
            --   * the quest-log selection (GetQuestLogSelection) - written by
            --     quest-log clicks and embiggened-M clicks (SelectQuestLogEntry,
            --     WorldMapFrame.lua:1674);
            --   * the windowed-M saved selection
            --     (WORLDMAP_SETTINGS.selectedQuestId) - written by windowed-M
            --     clicks AND the map's own auto-select of the first quest frame
            --     (WorldMapFrame_SelectQuestFrame, WorldMapFrame.lua:1658/1453/1483).
            -- We mirror whichever register Blizzard's engine updated most
            -- recently, and distinguish a FRESH selection (made in the current
            -- zone context: if ineligible the marker hides and NEVER auto-tracks
            -- over an explicit choice) from a STALE one (predates a zone change:
            -- Blizzard auto-tracks the first M-list quest, except border/shared
            -- quests that stay listed in the M tracker and stay selected-hidden).
            local zoneEpoch = 0   -- incremented on login / zone change / teleport
            local selEpoch = -1   -- zoneEpoch when the current selection was made
            local lastLSel = 0    -- last quest-log-selected quest id (register 1)
            local lastMSel = 0    -- last world-map selectedQuestId (register 2)
            local lastSource = 0  -- winning register: 0 none, 1 quest log (L), 2 world map (M)

            local function CurrentBlizzardSelection()
                local mQid = 0
                local mSel = (WORLDMAP_SETTINGS and WORLDMAP_SETTINGS.selectedQuestId) or 0
                if mSel ~= 0 then mQid = mSel end
                local lQid = 0
                local idx = GetQuestLogSelection()
                if idx and idx > 0 then
                    -- GetQuestLogTitle returns (FrameXML QuestLogFrame.lua:401):
                    -- title, level, questTag, suggestedGroup, isHeader, isCollapsed,
                    -- isComplete, isDaily, questID, displayQuestID - so isHeader is
                    -- position 5 (NOT 4) and questID is position 9.
                    local _, _, _, _, isHeader, _, _, _, qid = GetQuestLogTitle(idx)
                    if not isHeader and qid and qid > 0 then lQid = qid end
                end
                -- Opening the world map re-asserts M as the active register even
                -- when the engine auto-selects the same quest (no change fires).
                local mapOpen = (WorldMapFrame and WorldMapFrame:IsShown()) or false
                if mapOpen and mQid ~= 0 then
                    lastSource = 2
                end
                if lQid ~= lastLSel then
                    lastLSel = lQid
                    lastMSel = mQid
                    if lQid ~= 0 then
                        selEpoch = zoneEpoch
                        lastSource = 1
                    end
                    return lQid, (lQid ~= 0)
                end
                if mQid ~= lastMSel then
                    lastMSel = mQid
                    if mQid ~= 0 then
                        selEpoch = zoneEpoch
                        lastSource = 2
                    end
                    return mQid, (mQid ~= 0)
                end
                -- The register the player last used keeps winning while stable,
                -- so an L click is not reverted by a persisted M register one
                -- frame later. Baseline (no source yet, e.g. login): M then L.
                if lastSource == 1 then
                    return lQid, (selEpoch == zoneEpoch)
                end
                if lastSource == 2 then
                    return mQid, (selEpoch == zoneEpoch)
                end
                if mQid ~= 0 then return mQid, (selEpoch == zoneEpoch) end
                return lQid, (selEpoch == zoneEpoch)
            end

            -- True iff the quest is still listed in the current zone's M quest
            -- list. Border/shared-zone quests stay listed (e.g. the Westfall quest
            -- remains in Elwynn's M list even though it is not renderable there).
            local function IsQuestInMapList(qid)
                if not QuestMapUpdateAllQuests or not QuestPOIGetQuestIDByVisibleIndex then
                    return false
                end
                local now = GetTime()
                if now - poiRefreshTime > 0.5 then
                    poiRefreshTime = now
                    QuestMapUpdateAllQuests()
                end
                for i = 1, 64 do
                    local q, qIndex = QuestPOIGetQuestIDByVisibleIndex(i)
                    if not qIndex or qIndex <= 0 then return false end
                    if q == qid then return true end
                end
                return false
            end

            local function SyncToSelection()
                if zoneRefreshing then return end
                local selID, fresh = CurrentBlizzardSelection()
                if selID ~= 0 then
                    if cache[selID] then
                        if currentQuest ~= selID then
                            SwitchTo(selID, true)
                        end
                        return
                    end
                    -- Selected quest is not renderable in this zone (the server
                    -- does not stream marker data here) — BUT the marker never
                    -- owns the quest-info stream. The selected quest's kill/
                    -- interaction entries are zone-independent (the server computes
                    -- them from quest_template), so the quest-mob highlight must
                    -- follow the selection even when the marker is hidden.
                    -- Request its kill entries; never clear them just because the
                    -- marker hides.
                    RequestKillEntriesIfChanged(selID)
                    if fresh then
                        -- Explicit selection made in this zone context that is not
                        -- eligible: HIDE and keep hidden. The marker never auto-
                        -- tracks over an explicit choice (e.g. selecting a
                        -- Darkshore quest from the Quest Log while in Elwynn).
                        if currentQuest ~= 0 then
                            ClearAllQuestMarkers()
                            currentQuest = 0
                        end
                        return
                    end
                    -- Stale selection (predates a zone change / login / teleport):
                    -- mirror Blizzard. A border/shared quest still listed in the M
                    -- tracker stays selected but hidden (Westfall quest while in
                    -- Elwynn); a quest that left the zone entirely means Blizzard
                    -- auto-tracks the first M-list quest.
                    if IsQuestInMapList(selID) then
                        if currentQuest ~= 0 then
                            ClearAllQuestMarkers()
                            currentQuest = 0
                        end
                        return
                    end
                    selID = 0  -- fall through to Blizzard's auto-track below
                end
                -- No renderable selection: mirror Blizzard's auto-track of the
                -- first quest in the M list (the world map auto-selects frame 1).
                -- The marker follows that quest, it does not invent one.
                if selID == 0 then
                    selID = FirstMapListQuestId()
                end
                if selID ~= 0 then
                    -- Auto-tracked quest (first M-list quest) is SELECTED even if
                    -- it has no marker data here — keep its kill entries live.
                    RequestKillEntriesIfChanged(selID)
                    if cache[selID] then
                        if currentQuest ~= selID then
                            SwitchTo(selID, true)
                        end
                        return
                    end
                    return
                end
                -- Nothing eligible: hide. The marker NEVER promotes another quest.
                if currentQuest ~= 0 then
                    ClearAllQuestMarkers()
                    currentQuest = 0
                    ClearKillEntries()
                end
            end
        )lua"
        R"lua(


            -- Objective-tracker diamond: a small copy of the quest-marker icon
            -- (same atlas, same UVs as the world marker draw) shown to the left of
            -- the currently selected quest's title in the World Objective Tracker
            -- (WatchFrame). It mirrors currentQuest exactly - the marker system
            -- never picks a quest itself. It is RENDERED BY THE MODULE'S OWN D3D
            -- path (SetTrackerDiamond -> screen-space NDC quad with linear
            -- filtering): the UI SetTexture sampler aliases the 28x35 atlas cell
            -- at small sizes because the BLP has no mipmaps, and the 32x32 quest
            -- POI number buttons cover the line's $parentIcon spot anyway. The
            -- quad reuses the SAME IngameNavigationUI BLP and SAME texcoords as
            -- the world marker, placed just left of the POI button.
            local trackerDiamondHooked = false

            local function DiamondHide()
                if SetTrackerDiamond then SetTrackerDiamond(0, 0, 0, 0, 0) end
            end

            local function PlaceTrackerDiamond()
                if not WatchFrame or not WATCHFRAME_LINKBUTTONS then
                    DiamondHide()
                    return
                end
                -- Hidden tracker (collapsed or disabled in the UI) -> no diamond.
                if not WatchFrame:IsShown() or (WatchFrameLines and not WatchFrameLines:IsShown()) then
                    DiamondHide()
                    return
                end
                -- The tracker enable toggle governs the diamond too (icon + text + diamond).
                if not wxqmEnabled then DiamondHide(); return end
                if not currentQuest or currentQuest == 0 then DiamondHide(); return end
                for _, btn in ipairs(WATCHFRAME_LINKBUTTONS) do
                    if btn.type == "QUEST" and btn.lines and btn.startLine then
                        -- WXL: linkButton.index is the WATCH index (i), not the quest
                        -- log index -- map through GetQuestIndexForWatch() first.
                        local questLogIndex = btn.index and GetQuestIndexForWatch(btn.index)
                        if questLogIndex and questLogIndex > 0 then
                            local qid = select(9, GetQuestLogTitle(questLogIndex))
                            if qid and qid == currentQuest then
                                local line = btn.lines[btn.startLine]
                                if line then
                                    -- Auto-highlight the tracked quest's dropdown
                                    -- button exactly like a player click (yellow
                                    -- glow) on login / zone change / quest switch.
                                    -- Idempotent: QuestPOI_SelectButton early-returns
                                    -- when that quest is already the selected one.
                                    -- Gate on an actual register change: the native
                                    -- WatchFrame_Update re-selects its own quest and
                                    -- would otherwise ping-pong the selection (and
                                    -- the 101/103 request stream) between the M and
                                    -- L registers every frame.
                                    if QuestPOI_SelectButtonByQuestId and
                                       (not WORLDMAP_SETTINGS or
                                        WORLDMAP_SETTINGS.selectedQuestId ~= currentQuest) then
                                        QuestPOI_SelectButtonByQuestId('WatchFrameLines', currentQuest, true)
                                    end
                                    -- Convert the title row anchor to NDC. Diamond is
                                    -- 16x20 (arrow aspect 0.8), right edge 2px left
                                    -- of the 32px POI button, centered on the row.
                                    -- NDC math is scale-independent (UI px / UI screen).
                                    local sw = GetScreenWidth() or 1024
                                    local sh = GetScreenHeight() or 768
                                    local left = line:GetLeft() or 0
                                    local top = line:GetTop() or 0
                                    local bottom = line:GetBottom() or 0
                                    local cx = (left - 42) / sw * 2 - 1
                                    local cy = ((top + bottom) / 2 - 1) / sh * 2 - 1
                                    if SetTrackerDiamond then
                                        -- 1/0, NOT true/false: the C++ side reads args
                                        -- with lua_toNumber, which returns 0 for booleans.
                                        SetTrackerDiamond(1, cx, cy, 16 / sw, 20 / sh)
                                    end
                                end
                                return  -- exactly one diamond
                            end
                        end
                    end
                end
                DiamondHide()
            end
        )lua"
        R"lua(


            OnCustomPacket(100, function(reader)
                -- Quests are not blocked by packetLockout — marker data updates are safe
                -- to process immediately. The packetLockout was designed to protect render
                -- state during zone transitions, but OnCustomPacket(100) only updates a Lua
                -- cache and calls C++ SetMarker (data update, not rendering).
                -- Ignore when not in-world (character select / logout)
                if not UnitGUID("player") then return end
                local qid = reader:ReadUInt32()
                local active = reader:ReadUInt32()
                local x = reader:ReadDouble()
                local y = reader:ReadDouble()
                local z = reader:ReadDouble()
                reader:ReadUInt32()
                -- Server batch-complete sentinel (questId == 0): the marker stream
                -- for this refresh (login / /reload / zone change) is finished.
                -- Finalize deterministically: settle on the first M-menu tracked
                -- quest now instead of racing a fixed timer against late packets.
                if qid == 0 then
                    if loginBatch or zoneRefreshing then
                        if batchTimer then
                            batchTimer:SetScript("OnUpdate", nil)
                            batchTimer = nil
                        end
                        if showTimer then
                            showTimer:SetScript("OnUpdate", nil)
                            showTimer = nil
                        end
                        zoneRefreshing = false
                        loginBatch = false
                        SyncToSelection()
                    end
                    return
                end
                if active ~= 0 then
                    local wasFirst = (next(cache) == nil)
                    cache[qid] = {x, y, z}
                    if loginBatch then
                        if wasFirst then
                            -- Start batch timer NOW (from first actual marker arrival, not from PEW).
                            -- Don't show a marker immediately — wait until batch completes,
                            -- then SyncToSelection() mirrors the M tracker's selected quest.
                            if not batchTimer then
                                batchTimer = CreateFrame("Frame")
                            end
                            batchTimer.elapsed = 0
                            batchTimer:SetScript("OnUpdate", function(self, dt)
                                self.elapsed = self.elapsed + dt
                                -- 1.5s covers both the sendRetry response (~0.4s) and the
                                -- server's 1.2s login scan (~1.4s). All markers from both
                                -- batches arrive while loginBatch is still true, so only
                                -- SyncToSelection fires at expiry.
                                if self.elapsed < 1.5 then return end
                                self:SetScript("OnUpdate", nil)
                                loginBatch = false
                                -- After batch complete, mirror the M tracker's selected quest
                                SyncToSelection()
                            end)
                        end
                        -- All following markers during batch: cache silently, no visual switch
                        return
                    end
                    -- Marker data cached silently; the per-frame SyncToSelection
                    -- mirrors Blizzard's selection and renders it only if this
                    -- quest IS the selected one. The marker never switches on its own.
                    if zoneRefreshing then return end
                else
                    cache[qid] = nil  -- remove from cache so quests not in this zone don't reappear
                    if currentQuest == qid then
                        -- The marker's quest is no longer active: hide the marker and
                        -- re-mirror the M tracker selection (never auto-promote).
                        currentQuest = 0
                        ClearAllQuestMarkers()
                        ClearKillEntries()
                        SyncToSelection()
                    end
                end
            end)

            -- Objectives-tracker click: select the quest IN PLACE (mirror the
            -- map's selection registers) instead of opening the world map.
            -- The marker mirrors these registers every frame, so it follows.
            local function WXQM_TrackerPOI_OnClick(self)
                if not (self and self.questId and self.questId > 0) then
                    return
                end
                local alreadySelected = (WORLDMAP_SETTINGS and WORLDMAP_SETTINGS.selectedQuestId == self.questId)
                if WORLDMAP_SETTINGS then
                    WORLDMAP_SETTINGS.selectedQuestId = self.questId
                end
                if QuestPOI_SelectButtonByQuestId then
                    QuestPOI_SelectButtonByQuestId('WatchFrameLines', self.questId, true)
                end
                -- Mirror the quest-log register too so the L highlight follows.
                for i = 1, GetNumQuestLogEntries() do
                    local _, _, _, _, isHeader, _, _, _, qid = GetQuestLogTitle(i)
                    if not isHeader and qid == self.questId then
                        SelectQuestLogEntry(i)
                        break
                    end
                end
                -- Native click feedback only when the selection actually changes.
                if not alreadySelected then
                    PlaySound('igMainMenuOptionCheckBoxOn')
                end
            end
            -- POI buttons capture this global at creation time; re-assert on
            -- every PEW below so buttons created after a relog use it too.
            WatchFrameQuestPOI_OnClick = WXQM_TrackerPOI_OnClick

            local frame = CreateFrame('Frame')
            frame:RegisterEvent('PLAYER_ENTERING_WORLD')
            frame:RegisterEvent('PLAYER_LEAVING_WORLD')
            frame:SetScript('OnEvent', function(self, event)
                if event == 'PLAYER_LEAVING_WORLD' then
                    ClearAllQuestMarkers()
                    ClearCorpseMarker()
                    SetDeadState(0)
                    cache = {}
                    currentQuest = 0
                    -- Reset the kill-entries dedupe: on re-login the auto-tracked
                    -- quest must re-request its quest-info (the server is stateless
                    -- per session and selcircle clears its store on world leave).
                    lastKillQuest = 0
                    loginBatch = true
                    zoneRefreshing = false
                    if batchTimer then
                        batchTimer:SetScript("OnUpdate", nil)
                    end
                    if showTimer then
                        showTimer:SetScript("OnUpdate", nil)
                        showTimer = nil
                    end
                    return
                end
                                -- PEW fires on EVERY login (logout to char select and back). Keep the frame
                -- registered and re-arm the marker flow here instead of unregistering.
                -- Otherwise a re-login never re-requests markers and the client
                -- "forgets" the quests it has (no fresh markers, stale login state).
                loginBatch = true
                zoneRefreshing = false
                cache = {}
                currentQuest = 0
                poiRefreshTime = 0
                zoneEpoch = zoneEpoch + 1
                WatchFrameQuestPOI_OnClick = WXQM_TrackerPOI_OnClick
                -- Absorb Blizzard's current selection registers as the baseline
                -- WITHOUT marking them fresh: a selection persisted from a previous
                -- session/zone is STALE here. If it is renderable (the server
                -- streams it) the marker shows it - the marker is "remembered"
                -- across logins. If not, Blizzard auto-tracks the first M-list
                -- quest and the marker mirrors that instead of hiding forever.
                lastMSel = (WORLDMAP_SETTINGS and WORLDMAP_SETTINGS.selectedQuestId) or 0
                lastSource = 0  -- no register wins at login; baseline is stale
                local _li = GetQuestLogSelection()
                local _lq = 0
                if _li and _li > 0 then
                    local _, _, _, _, _h, _, _, _, _qid = GetQuestLogTitle(_li)
                    if not _h and _qid and _qid > 0 then _lq = _qid end
                end
                lastLSel = _lq
                if batchTimer then
                    batchTimer:SetScript("OnUpdate", nil)
                    batchTimer = nil
                end
                if showTimer then
                    showTimer:SetScript("OnUpdate", nil)
                    showTimer = nil
                end
                if not initialized then
                    initialized = true
)lua" R"lua(
                    -- Place the tracker diamond whenever the Objective Tracker
                    -- rebuilds its rows (add/remove/reorder/collapse). Selection
                    -- moves without a rebuild are covered by the per-frame call
                    -- in the deathFrame OnUpdate below. Wrapped in pcall so a hook
                    -- failure can never abort the rest of the login handler, and
                    -- retried until WatchFrame_Update exists (lazy FrameXML load).
                    local function wxHookDiamond()
                        if not trackerDiamondHooked and hooksecurefunc and WatchFrame_Update then
                            trackerDiamondHooked = pcall(hooksecurefunc, "WatchFrame_Update", PlaceTrackerDiamond)
                        end
                        return trackerDiamondHooked
                    end
                    if not wxHookDiamond() then
                        local hookRetry = CreateFrame("Frame")
                        hookRetry.elapsed = 0
                        hookRetry:SetScript("OnUpdate", function(self, dt)
                            self.elapsed = self.elapsed + dt
                            if wxHookDiamond() or self.elapsed > 10 then
                                self:SetScript("OnUpdate", nil)
                            end
                        end)
                    end
                                end  -- initialized: slash cmds / zoneChange / deathFrame / hooks are one-time

                -- Request server to resend all quest markers (handles /reload and re-login).
                -- Runs on EVERY login so a re-login gets fresh markers just like the first one.
                -- Deferred by 0.2s past PEW to let world state settle.
                -- packetLockout no longer blocks opcode 100, so markers arrive ASAP.
local sendRetry = CreateFrame("Frame")
                sendRetry.elapsed = 0
                sendRetry:SetScript("OnUpdate", function(self, dt)
                    self.elapsed = self.elapsed + dt
                    if self.elapsed < 0.2 then return end
                    self:SetScript("OnUpdate", nil)
                    local rpkt = CreateCustomPacket(100, 2)
                    rpkt:WriteUInt16(102)
                    rpkt:Send()
                end)

                -- SyncToSelection is defined at chunk level above.
                -- Both the batch timer (in OnCustomPacket) and the PEW handler share it.
                -- No local redefinition needed here.
        )lua"
        R"lua(
                -- Zone change: clear old marker, then request fresh ones
                local zoneChange = CreateFrame('Frame')
                zoneChange:RegisterEvent('ZONE_CHANGED_NEW_AREA')
                zoneChange:SetScript('OnEvent', function()
                    -- Engage packet guard to prevent stale marker data during transition
                    if SetPacketLockout then SetPacketLockout() end
                    -- Skip marker refresh during login batch (login already handles it)
                    if loginBatch then return end
                    -- Clear all cached markers immediately — old zone's quests are stale
                    cache = {}
                    ClearAllQuestMarkers()
                    currentQuest = 0
                    ClearKillEntries()
                    -- Zone change only affects ELIGIBILITY. The cache is cleared and
                    -- the server re-streams only quests eligible in the new zone. The
                    -- marker then mirrors Blizzard's selection: a FRESH selection is
                    -- hidden if not renderable here; a STALE one (predates the zone
                    -- change) follows Blizzard - border quests stay selected-hidden,
                    -- everything else auto-tracks the first M-list quest.
                    zoneEpoch = zoneEpoch + 1
                    poiRefreshTime = 0
                    -- Set zone refreshing flag to suppress auto-switch on new markers
                    zoneRefreshing = true
                    -- Request fresh markers for the new zone after a short settle delay
                    local retry = CreateFrame("Frame")
                    retry.elapsed = 0
                    retry:SetScript("OnUpdate", function(self, dt)
                        self.elapsed = self.elapsed + dt
                        if self.elapsed < 0.5 then return end
                        self:SetScript("OnUpdate", nil)
                        local pkt = CreateCustomPacket(100, 2)
                        pkt:WriteUInt16(102)
                        pkt:Send()
                        -- After markers arrive, show the first watched M-menu quest.
                        -- IMPORTANT: the server's opcode-102 handler scans ALL quest
                        -- ids and streams markers back over a second+. The old fixed
                        -- 1.0s timer fired before the FIRST marker arrived, so
                        -- SyncToSelection ran on an empty cache and the marker
                        -- ended up on whichever quest's packet arrived last (the
                        -- server re-sends the first quest last) -- forcing the player
                        -- to press M to re-select the tracked quest.
                        -- Mirror the login batch instead: wait for the first marker,
                        -- then give the batch 1.5s to complete before switching.
                        showTimer = CreateFrame("Frame")
                        showTimer.started = false
                        showTimer.elapsed = 0
                        showTimer:SetScript("OnUpdate", function(self2, dt2)
                            if not showTimer then return end
                            if not showTimer.started then
                                if next(cache) == nil then
                                    -- No markers yet: keep waiting (hard timeout so the
                                    -- refresh can never hang on a silent server).
                                    showTimer.elapsed = showTimer.elapsed + dt2
                                    if showTimer.elapsed > 6.0 then
                                        self2:SetScript("OnUpdate", nil)
                                        showTimer = nil
                                        zoneRefreshing = false
                                        SyncToSelection()
                                    end
                                    return
                                end
                                showTimer.started = true
                                showTimer.elapsed = 0
                                return
                            end
                            showTimer.elapsed = showTimer.elapsed + dt2
                            if showTimer.elapsed < 1.5 then return end
                            self2:SetScript("OnUpdate", nil)
                            showTimer = nil
                            zoneRefreshing = false
                            -- Zone change complete: mirror the M tracker's selected quest
                            -- of the new zone, or clear the marker if it is not available.
                            SyncToSelection()
                        end)
                    end)
                end)

                local deathFrame = CreateFrame('Frame')
                deathFrame:SetSize(1, 1)
                deathFrame:Show()
                -- Corpse marker from server (opcode 300). Not blocked by packetLockout:
                -- it only updates marker data (SetCorpseMarker), like opcode 100, so it is
                -- safe to process immediately - and it MUST arrive during the login window
                -- (the server sends it on login while dead) or the corpse icon would not
                -- persist across log out / log in.
                OnCustomPacket(300, function(reader)
                    local active = reader:ReadUInt32()
                    if active ~= 0 then
                        local x = reader:ReadDouble()
                        local y = reader:ReadDouble()
                        local z = reader:ReadDouble()
                        SetCorpseMarker(x, y, z)
                    else
                        ClearCorpseMarker()
                    end
                end)

                -- Per-frame death state detection + corpse position capture
                local prevDead = UnitIsDeadOrGhost("player") and true or false
                if prevDead then
                    SetDeadState(1)
                end
                deathFrame:SetScript('OnUpdate', function()
                    local nowDead = UnitIsDeadOrGhost("player")
                    if nowDead and not prevDead then
                        local x, y, z = GetPlayerPosition()
                        if x and x ~= 0 then SetCorpseMarker(x, y, z) end
                    elseif nowDead and not IsCorpseActive() then
                        -- Dead but no corpse marker armed yet (login while dead, or the
                        -- death transition fired before the player object resolved).
                        -- Retry the player position - a ghost logs in at its corpse
                        -- location - until the server's authoritative position arrives
                        -- or this capture lands.
                        local x, y, z = GetPlayerPosition()
                        if x and x ~= 0 then SetCorpseMarker(x, y, z) end
                    elseif not nowDead and prevDead then
                        ClearCorpseMarker()
                    end
                    prevDead = nowDead
                    SetDeadState(nowDead and 1 or 0)
                    -- Mirror Blizzard's current selection every frame (idempotent).
                    -- Retrying every frame covers the async quest-log load at login and
                    -- picks up any selection change in L/M within a frame.
                    -- SyncToSelection renders ONLY the selected quest when it is cached
                    -- (eligible), hides a fresh ineligible selection, and mirrors
                    -- Blizzard's first-M-list-quest auto-track for stale ones; it never
                    -- promotes. Skip during login batch - batch completion finalizes
                    -- via SyncToSelection.
                    if not loginBatch then
                        SyncToSelection()
                    end
                    -- Objective tracker diamond: mirror the current selection every
                    -- frame so M/L/objective-tracker clicks move it immediately
                    -- (WatchFrame only rebuilds on its own events).
                    PlaceTrackerDiamond()
                    -- No auto-promotion: marker stays hidden until M tracker selection changes
                end)
                -- The "Track Quest" checkbox (AddQuestWatch/RemoveQuestWatch) does NOT
                -- change Blizzard's selection registers, so it does not drive the
                -- marker (only quest-log / map clicks do, mirrored every frame).
)lua"
        R"lua(                -- Distance text frame
                if not distFrame then
                    distFrame = CreateFrame("Frame", nil, UIParent)
                    distFrame:SetSize(200, 30)
                    distFrame:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
                    distFrame:SetFrameStrata("HIGH")
                    distText = distFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
                    distText:SetFont("Interface\\AddOns\\ElvUI\\Media\\Fonts\\Expressway.ttf", 14)
                    distText:SetPoint("TOP", distFrame, "TOP", 0, 0)
                    distText:SetTextColor(1, 0.84, 0, wxqmMarkerAlpha / 255)
                    distText:SetShadowOffset(1, -1)
                    -- Corpse distance text frame (separate from quest distance)
                    local corpseFrame = CreateFrame("Frame", nil, UIParent)
                    corpseFrame:SetSize(200, 30)
                    corpseFrame:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
                    corpseFrame:SetFrameStrata("HIGH")
                    local corpseText = corpseFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
                    corpseText:SetFont("Interface\\AddOns\\ElvUI\\Media\\Fonts\\Expressway.ttf", 14)
                    corpseText:SetPoint("TOP", corpseFrame, "TOP", 0, 0)
                    corpseText:SetTextColor(1, 0.84, 0, 1.0)
                    corpseText:SetShadowOffset(1, -1)
                    corpseFrame:SetScript("OnUpdate", function()
                        if not IsCorpseActive() then
                            corpseText:SetText("")
                            return
                        end
                        local ndcX, ndcY, dist = GetCorpseDistInfo()
                        if ndcX then
                            corpseText:SetText("Corpse - " .. formatDist(dist))
                            local sw, sh = GetScreenWidth(), GetScreenHeight()
                            local uiX = math.max(0, math.min(sw, (ndcX + 1) * 0.5 * sw))
                            local uiY = math.max(0, math.min(sh, (ndcY + 1) * 0.5 * sh))
                            corpseFrame:ClearAllPoints()
                            corpseFrame:SetPoint("TOP", UIParent, "BOTTOMLEFT", uiX, uiY - 20)
                        else
                            corpseText:SetText("")
                        end
                    end)

                    -- Quest distance text frame -- hides when corpse is active
                    distFrame:SetScript("OnUpdate", function()
                        distText:SetTextColor(1, 0.84, 0, wxqmMarkerAlpha / 255)
                        if not currentQuest or currentQuest == 0 then
                            distText:SetText("")
                            return
                        end
                        if not wxqmEnabled then
                            distText:SetText("")
                            return
                        end
                        if IsCorpseActive() then
                            distText:SetText("")
                            return
                        end
                        local c = cache[currentQuest]
                        if not c then
                            -- Marker data missing (zone refresh in progress / quest gone):
                            -- clear the text instead of leaving the previous frame's text.
                            distText:SetText("")
                            return
                        end
                        local ndcX, ndcY, dist = GetDistInfo()
                        if ndcX then
                            -- Distance ceiling: hide text for very far quests (>5000 yd)
                            -- Zone changes may take a moment to refresh markers from server
                            if dist > 5000 then
                                distText:SetText("")
                                return
                            end
                            distText:SetText(formatDist(dist))
                            local sw, sh = GetScreenWidth(), GetScreenHeight()
                            local uiX = math.max(0, math.min(sw, (ndcX + 1) * 0.5 * sw))
                            local uiY = math.max(0, math.min(sh, (ndcY + 1) * 0.5 * sh))
                            distFrame:ClearAllPoints()
                            distFrame:SetPoint("TOP", UIParent, "BOTTOMLEFT", uiX, uiY - 20)
                        else
                            -- No marker drawn this frame (stale m_markerValid was cleared):
                            -- clear the text so it never lingers without an icon.
                            distText:SetText("")
                        end
                    end)
                end
                -- Startup: push initial settings to C++
                local function StartupSettings()
                    if not QuestTrackerReloadSettings then return end
                    QuestTrackerReloadSettings(
                        GetCVar("wxTrackerEnabled") == "1" and 1 or 0,
                        GetCVar("wxTrackerShowMarker") == "1" and 1 or 0,
                        tonumber(GetCVar("wxTrackerAlpha")) or 165
                    )
                    wxqmEnabled = GetCVar("wxTrackerEnabled") == "1"
                    wxqmMarkerAlpha = tonumber(GetCVar("wxTrackerAlpha")) or 165
                end
                StartupSettings()
            end)
        )lua");

        wxl::runtime::lua::RegisterScript("wxl-quest-marker-panels", R"lua(
            local frame = CreateFrame("Frame")
            frame:RegisterEvent("PLAYER_ENTERING_WORLD")
            frame:RegisterEvent("PLAYER_LEAVING_WORLD")
            frame:SetScript("OnEvent", function(self, event)
                if event == "PLAYER_LEAVING_WORLD" then return end
                frame:UnregisterAllEvents()
                frame:SetScript("OnEvent", nil)

                local retry = CreateFrame("Frame")
                local n = 0
                retry:SetScript("OnUpdate", function()
                    n = n + 1
                    if not TriAddCategory then
                        if n > 300 then retry:SetScript("OnUpdate", nil) end
                        return
                    end
                    retry:SetScript("OnUpdate", nil)

                    local function AS()
                        QuestTrackerReloadSettings(
                            GetCVar("wxTrackerEnabled") == "1" and 1 or 0,
                            GetCVar("wxTrackerShowMarker") == "1" and 1 or 0,
                            tonumber(GetCVar("wxTrackerAlpha")) or 165
                        )
                        wxqmEnabled = GetCVar("wxTrackerEnabled") == "1"
                        wxqmMarkerAlpha = tonumber(GetCVar("wxTrackerAlpha")) or 165
                    end

                    local panel = CreateFrame("Frame", "WXTrackerPanel")
                    panel.name = "Objective Tracker"
                    panel.okay = function() end
                    panel.cancel = function()
                        pcall(function()
                            _G["WXTrackerEnabled"]:SetChecked(GetCVar("wxTrackerEnabled") == "1")
                        end)
                    end
                    panel.default = function()
                        SetCVar("wxTrackerEnabled", "1")
                        SetCVar("wxTrackerAlpha", "165")
                        pcall(function()
                            _G["WXTrackerEnabled"]:SetChecked(true)
                        end)
                        pcall(AS)
                    end
                    panel.refresh = function()
                        pcall(function()
                            local a = tonumber(GetCVar("wxTrackerAlpha")) or 165
                            _G["WXTrackerEnabled"]:SetChecked(GetCVar("wxTrackerEnabled") == "1")
                            local s = _G["WXOptSlider"]; if s then s:SetValue(a) end
                            local v = _G["WXOptSliderValue"]; if v then v:SetText(tostring(a)) end
                        end)
                    end

                    local title = panel:CreateFontString(nil, "ARTWORK")
                    title:SetFont("Fonts\\FRIZQT__.TTF", 14)
                    title:SetText("Objective Tracker")
                    title:SetPoint("TOPLEFT", 16, -16)
                    local desc = panel:CreateFontString(nil, "ARTWORK")
                    desc:SetFont("Fonts\\FRIZQT__.TTF", 12)
                    desc:SetText("Configure the 3D Quest Tracker.")
                    desc:SetPoint("TOPLEFT", 16, -38)
                    desc:SetWidth(400)
                    desc:SetJustifyH("LEFT")

                    local cb = CreateFrame("CheckButton", "WXTrackerEnabled", panel, "InterfaceOptionsCheckButtonTemplate")
                    cb:SetPoint("TOPLEFT", desc, "BOTTOMLEFT", -2, -16)
                    _G["WXTrackerEnabledText"]:SetText("Enable Quest Tracker")
                    cb:SetScript("OnClick", function()
                        SetCVar("wxTrackerEnabled", cb:GetChecked() and "1" or "0"); pcall(AS)
                    end)

                    local al = panel:CreateFontString(nil, "ARTWORK")
                    al:SetFont("Fonts\\FRIZQT__.TTF", 12)
                    al:SetText("Opacity")
                    al:SetPoint("TOPLEFT", cb, "BOTTOMLEFT", 4, -52)
                    local sl = CreateFrame("Slider", "WXOptSlider", panel, "OptionsSliderTemplate")
                    sl:SetWidth(180)
                    sl:SetPoint("TOPLEFT", al, "BOTTOMLEFT", -4, -16)
                    sl:SetMinMaxValues(0, 255)
                    sl:SetValueStep(1)
                    local av = panel:CreateFontString("WXOptSliderValue", "ARTWORK")
                    av:SetFont("Fonts\\FRIZQT__.TTF", 12)
                    av:SetPoint("LEFT", sl, "RIGHT", 8, 0)
                    sl:SetScript("OnValueChanged", function(self, v)
                        local val = math.floor(v + 0.5)
                        av:SetText(tostring(val))
                        SetCVar("wxTrackerAlpha", tostring(val)); pcall(AS)
                    end)

                    pcall(TriAddCategory, panel)
                    pcall(panel.refresh, panel)
                    pcall(AS)
                end)
            end)
        )lua");
    }    struct Registrar
    {
        Registrar()
        {
            wxl::runtime::modules::Register("wxl-quest-marker", &InstallModule);
        }
    } g_registrar;
    #endif

    QuestMarker::QuestMarker()
    {
        on<&QuestMarker::OnWorldRenderEnd>(wxl::events::Event::OnWorldRenderEnd);
        on<&QuestMarker::OnWorldLeave>(wxl::events::Event::OnWorldLeave);
        on<&QuestMarker::OnWorldEnter>(wxl::events::Event::OnWorldEnter);
        on<&QuestMarker::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
        on<&QuestMarker::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
    }

    QuestMarker::~QuestMarker() { ReleaseDevice(); }

    void QuestMarker::SetCorpseMarker(float x, float y, float z)
    {
        m_corpseActive = true;
        m_corpseX = x; m_corpseY = y; m_corpseZ = z;
    }

    void QuestMarker::ClearCorpseMarker()
    {
        m_corpseActive = false;
    }

    void QuestMarker::SetMarker(uint32_t questId, bool active,
                                float x, float y, float z)
    {
        if (active) {
            for (auto& m : m_markers)
                if (m.questId == questId) {
                    m.active = true; m.x = x; m.y = y; m.z = z;
                    return;
                }
            m_markers.push_back({questId, true, x, y, z});
        } else {
            for (auto it = m_markers.begin(); it != m_markers.end(); ++it)
                if (it->questId == questId) { m_markers.erase(it); break; }
        }
    }

    void QuestMarker::ClearAllMarkers()
    {
        m_markers.clear();
    }

    void QuestMarker::OnDeviceLost(const wxl::events::DeviceResetArgs&)
    {
        ReleaseDevice();
        WLOG_INFO("device-lost resources released");
    }
    void QuestMarker::OnDeviceReset(const wxl::events::DeviceResetArgs& a) {
        if (a.device) InitDevice(a.device);
    }

    void QuestMarker::OnWorldLeave(const wxl::events::WorldLeaveArgs&)
    {
        ClearAllMarkers();
        ClearCorpseMarker();
        m_deadState = false;
        m_markerValid = false;
        m_corpseNdcValid = false;
        ReleaseDevice();
    }

    void QuestMarker::OnWorldEnter(const wxl::events::WorldEnterArgs&)
    {
        m_worldEnterFrameCount = 0;
        // Fresh world session: any markers surviving from a previous session are
        // stale. Clear so the next login starts from a clean slate.
        m_markers.clear();
        ClearCorpseMarker();
        m_deadState = false;
        m_markerValid = false;
        m_corpseNdcValid = false;
        m_trackerDiamondActive = false;
        InitDevice(wxl::game::gx::RawDevice());
    }

    void QuestMarker::InitDevice(void* device)
    {
        if (m_deviceReady) return;

        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!d) { m_deviceReady = true; return; }

        m_d3dTexture = LoadBlpTexture(d, "interface\\navigation\\ingamenavigationui.blp");
        if (!m_d3dTexture)
        {
            m_d3dTexture = CreateDebugTexture(d);
        }
        m_deviceReady = true;
    }

    void QuestMarker::ReleaseDevice()
    {
        FreeTexture(m_d3dTexture);
        m_d3dTexture = nullptr;

        if (m_vb) {
            static_cast<IDirect3DVertexBuffer9*>(m_vb)->Release();
            m_vb = nullptr;
            m_vbSize = 0;
        }
        m_deviceReady = false;
    }

    void QuestMarker::OnWorldRenderEnd(const wxl::events::WorldRenderEndArgs& a)
    {
        // Clear the distance-info validity whenever no marker will be drawn this
        // frame. Leaving m_markerValid true here makes GetDistInfo() return stale
        // NDC/dist after a zone change clears all markers, so the Lua distance
        // text keeps rendering "phantom" distance without any icon.
        if (m_markers.empty() && !m_corpseActive && !m_trackerDiamondActive) { m_markerValid = false; return; }
        if (!m_settings.enabled && !m_corpseActive && !m_trackerDiamondActive) { m_markerValid = false; return; }
        if (!m_settings.showMarker && m_markers.empty() && !m_trackerDiamondActive) { m_markerValid = false; return; }

        // Not in-world (character select / login screen / logout).
        // Stale marker state from a previous world session MUST be cleared here:
        // OnWorldLeave only fires on world *enter* transitions (CWorldEnter), so
        // a plain logout to character select leaves old markers alive in the
        // singleton. They would render as ghost markers with distance text on the
        // next login. Markers are only ever set while in-world (the Lua packet
        // handler gates on UnitGUID("player")), so any state present here is stale.
        if (world::CurrentMapId() < 0 || !world::ActivePlayerGuid())
        {
            m_markers.clear();
            ClearCorpseMarker();
            m_deadState = false;
            m_markerValid = false;
            m_corpseNdcValid = false;
            m_trackerDiamondActive = false;
            return;
        }

        // Use the engine's live camera matrices (cam::View / cam::Projection) for the
        // marker draw. The device transforms captured into SavedMarkerStates are NOT
        // the world camera at this point: OnWorldRenderEnd fires after the engine's
        // world finalize has reset the device state for the UI pass, so the captured
        // D3DTS_VIEW / D3DTS_PROJECTION are identity/UI matrices and the world-space
        // quads project off-screen (no marker at all). The engine camera globals are
        // the matrices the world pass was actually rendered with - the event even
        // passes a null projection in-world, expecting the subscriber to read these.

        gx::Device9 dev(a.device);
        if (!dev) { m_markerValid = false; return; }

        if (!m_deviceReady || !m_d3dTexture) InitDevice(a.device);
        if (!m_deviceReady || !m_d3dTexture)
        {
            m_markerValid = false;
            return;
        }

        auto* rawDev = static_cast<IDirect3DDevice9*>(dev.raw());

        SavedMarkerStates saved;
        saved.Capture(rawDev);

        rawDev->SetVertexShader(nullptr);
        rawDev->SetPixelShader(nullptr);
        rawDev->SetTexture(0, static_cast<IDirect3DBaseTexture9*>(m_d3dTexture));
        rawDev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
        rawDev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        rawDev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        rawDev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        rawDev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        rawDev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        rawDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        rawDev->SetRenderState(D3DRS_LIGHTING, FALSE);
        rawDev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        rawDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        rawDev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        rawDev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        rawDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        rawDev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        rawDev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        // Disable texture stages 1-7: the engine may leave multitexturing enabled
        // (e.g. terrain blending, WMO rendering). If stage 1 is still active with
        // D3DTOP_MODULATE and a stale texture, the marker's colors get multiplied
        // by that texture — causing black/dark rendering at distance where the
        // stale texture's lower mips are sampled. Disabling COLOROP + ALPHAOP
        // for all higher stages prevents this color bleeding completely.
        for (DWORD s = 1; s < 8; ++s)
        {
            rawDev->SetTextureStageState(s, D3DTSS_COLOROP, D3DTOP_DISABLE);
            rawDev->SetTextureStageState(s, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        }
        // No mip filtering — texture has only 1 mip level, D3DTEXF_LINEAR returns black
        rawDev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        rawDev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        rawDev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        // Draw with the engine camera matrices - the ones the world pass was rendered
        // with. Never read D3DTS_VIEW / D3DTS_PROJECTION from the device at the
        // world->UI boundary: they no longer hold the world camera there.
        const float* viewMat = cam::GetView();
        const float* projMat = cam::GetProjection();
        if (!viewMat || !projMat) { m_markerValid = false; saved.Restore(); return; }
        rawDev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(viewMat));
        // Extend far plane to prevent clipping at distance
        float projInf[16];
        memcpy(projInf, projMat, sizeof(projInf));
        float zn = -projMat[14] / projMat[10];
        float zf = 100000.0f;
        projInf[10] = zf / (zf - zn);
        projInf[14] = -zn * zf / (zf - zn);
        rawDev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(projInf));

        float camPos[3];
        cam::GetPosition(camPos);

        // Also skip if camera at origin (glue screen fallback)
        if (camPos[0] == 0.0f && camPos[1] == 0.0f && camPos[2] == 0.0f) { m_markerValid = false; saved.Restore(); return; }

        // Skip first 3 frames after world enter — camera vectors may be stale
        if (m_worldEnterFrameCount < 3) { ++m_worldEnterFrameCount; m_markerValid = false; saved.Restore(); return; }

        // Get player position for distance text
        float plrPos[3] = { camPos[0], camPos[1], camPos[2] };
        auto plrGuid = world::ActivePlayerGuid();
        if (plrGuid)
        {
            void* playerObj = world::ResolveObject(plrGuid, world::kTypeMaskPlayer);
            if (playerObj) world::UnitPosition(playerObj, plrPos);
        }

        // --- Pre-compute camera vectors for spherical billboard ---
        float rightX = viewMat[0], rightY = viewMat[4], rightZ = viewMat[8];
        float upX    = viewMat[1], upY    = viewMat[5], upZ    = viewMat[9];

        // Guard against degenerate billboard (camera not ready yet)
        if (rightX == 0.0f && rightY == 0.0f && rightZ == 0.0f) {
            m_markerValid = m_corpseNdcValid;
            saved.Restore();
            return;
        }

        D3DVIEWPORT9 vp;
        rawDev->GetViewport(&vp);
        float vpHeight = static_cast<float>(vp.Height);
        float yScale = projMat[5];

        // =====================================================================
        // SECTION 1: Corpse marker (completely separate from quest markers)
        // =====================================================================
        m_corpseNdcValid = false;
        if (m_corpseActive)
        {
            float viewZ = m_corpseX * viewMat[2] + m_corpseY * viewMat[6] + m_corpseZ * viewMat[10] + viewMat[14];

            // Compute NDC for corpse distance text
            float vx = m_corpseX*viewMat[0] + m_corpseY*viewMat[4] + m_corpseZ*viewMat[8]  + viewMat[12];
            float vy = m_corpseX*viewMat[1] + m_corpseY*viewMat[5] + m_corpseZ*viewMat[9]  + viewMat[13];
            float vz = m_corpseX*viewMat[2] + m_corpseY*viewMat[6] + m_corpseZ*viewMat[10] + viewMat[14];
            float vw = m_corpseX*viewMat[3] + m_corpseY*viewMat[7] + m_corpseZ*viewMat[11] + viewMat[15];
            float px = vx*projMat[0] + vy*projMat[4] + vz*projMat[8]  + vw*projMat[12];
            float py = vx*projMat[1] + vy*projMat[5] + vz*projMat[9]  + vw*projMat[13];
            float pw = vx*projMat[3] + vy*projMat[7] + vz*projMat[11] + vw*projMat[15];
            if (pw >= 0.0001f) {
                m_corpseNdcX = px / pw;
                m_corpseNdcY = py / pw;
            } else {
                m_corpseNdcX = (vx >= 0.0f) ? 2.0f : -2.0f;
                m_corpseNdcY = (vy >= 0.0f) ? 2.0f : -2.0f;
            }
            float cdx = m_corpseX - plrPos[0], cdy = m_corpseY - plrPos[1], cdz = m_corpseZ - plrPos[2];
            m_corpseDist = sqrtf(cdx*cdx + cdy*cdy + cdz*cdz);
            m_corpseNdcValid = true;

            // Draw corpse marker if in front of camera
            if (viewZ >= 0.1f)
            {
                float s = kMarkerPixelSize * 2.0f * viewZ / (yScale * vpHeight);

                // Corpse icon is intentionally drawn fully opaque (0xFF) - user preference.
                // Only the quest marker follows the opacity setting. The atlas's gravestone
                // art has translucent-black edge texels baked in (outer fringe alpha 59-61,
                // bottom band up to 184, border 171-255), which LINEAR filtering turns into
                // a pulsing dark fringe while strafing - the quest arrow avoids this because
                // its cell has transparent padding around the icon. Alpha-test them out
                // (keep alpha >= 160: the border, min 171, and interior, 255, survive;
                // the translucent fringe and most of the bottom band do not) so the icon
                // stays fully opaque with a clean edge.
                // Restored immediately so the quest draw is unaffected.
                const DWORD prevAlphaTest = saved.alphaTest;
                const DWORD prevAlphaFunc = saved.alphaFunc;
                const DWORD prevAlphaRef  = saved.alphaRef;
                rawDev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                rawDev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
                rawDev->SetRenderState(D3DRS_ALPHAREF, 160);
                MarkerVert cv[6];
                constexpr float kCorpseAspect = 28.0f / 34.0f;
                cv[0] = MarkerVert{ -kCorpseAspect,  1.0f, 0, 0xFFFFFFFFu, 0.002f,   0.002f   };
                cv[1] = MarkerVert{ -kCorpseAspect, -1.0f, 0, 0xFFFFFFFFu, 0.002f,   0.52825f };
                cv[2] = MarkerVert{  kCorpseAspect,  1.0f, 0, 0xFFFFFFFFu, 0.4355f, 0.002f   };
                cv[3] = MarkerVert{  kCorpseAspect,  1.0f, 0, 0xFFFFFFFFu, 0.4355f, 0.002f   };
                cv[4] = MarkerVert{ -kCorpseAspect, -1.0f, 0, 0xFFFFFFFFu, 0.002f,   0.52825f };
                cv[5] = MarkerVert{  kCorpseAspect, -1.0f, 0, 0xFFFFFFFFu, 0.4355f, 0.52825f };

                // Temp VB for corpse -- reuse m_vb but draw corpse first, then fill quest
                D3DMATRIX w = {};
                w._11 = rightX * s;  w._12 = rightY * s;  w._13 = rightZ * s;
                w._21 = upX * s;     w._22 = upY * s;     w._23 = upZ * s;
                w._31 = 0.0f;        w._32 = 0.0f;        w._33 = 0.0f;
                w._41 = m_corpseX;   w._42 = m_corpseY;   w._43 = m_corpseZ;   w._44 = 1.0f;
                rawDev->SetTransform(D3DTS_WORLD, &w);
                rawDev->SetFVF(D3DFVF_MARKERVERTS);
                rawDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, cv, sizeof(MarkerVert));

                // Restore the alpha-test state before the quest marker draw.
                rawDev->SetRenderState(D3DRS_ALPHATESTENABLE, prevAlphaTest);
                rawDev->SetRenderState(D3DRS_ALPHAFUNC, prevAlphaFunc);
                rawDev->SetRenderState(D3DRS_ALPHAREF, prevAlphaRef);
            }
        }

        // =====================================================================
        // SECTION 3: Objective-tracker diamond (screen-space NDC quad)
        // =====================================================================
        // Drawn by the module's own D3D path (linear filtering, mipfilter NONE)
        // because the UI SetTexture sampler aliases the 28x35 atlas cell at small
        // sizes (the BLP has no mipmaps). Same IngameNavigationUI texture, same
        // UVs and orientation as the world marker.
        if (m_trackerDiamondActive)
        {
            D3DMATRIX id = {};
            id._11 = id._22 = id._33 = id._44 = 1.0f;
            rawDev->SetTransform(D3DTS_WORLD, &id);
            rawDev->SetTransform(D3DTS_VIEW, &id);
            rawDev->SetTransform(D3DTS_PROJECTION, &id);

            // Always on top of the world at that pixel (no depth test).
            // Restore to D3DZB_TRUE (the value the shared setup established and
            // the marker draw relies on) rather than saved.zEnable, so the
            // diamond is provably behavior-neutral for the marker path.
            rawDev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);

            const float hw = m_trackerNdcHalfW;
            const float hh = m_trackerNdcHalfH;
            const float x0 = m_trackerNdcX - hw, x1 = m_trackerNdcX + hw;
            const float y0 = m_trackerNdcY - hh, y1 = m_trackerNdcY + hh;
            MarkerVert tv[6];
            tv[0] = MarkerVert{ x0, y1, 0.5f, 0xFFFFFFFFu, 0.4395f, 0.002f    };
            tv[1] = MarkerVert{ x0, y0, 0.5f, 0xFFFFFFFFu, 0.4395f, 0.543875f };
            tv[2] = MarkerVert{ x1, y1, 0.5f, 0xFFFFFFFFu, 0.8725f, 0.002f    };
            tv[3] = MarkerVert{ x1, y1, 0.5f, 0xFFFFFFFFu, 0.8725f, 0.002f    };
            tv[4] = MarkerVert{ x0, y0, 0.5f, 0xFFFFFFFFu, 0.4395f, 0.543875f };
            tv[5] = MarkerVert{ x1, y0, 0.5f, 0xFFFFFFFFu, 0.8725f, 0.543875f };
            rawDev->SetFVF(D3DFVF_MARKERVERTS);
            rawDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, tv, sizeof(MarkerVert));

            rawDev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);

            // The quest-marker section below only re-sets WORLD per marker and
            // still needs the camera transforms we just replaced, so put them
            // back before the marker draw.
            rawDev->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(viewMat));
            rawDev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(projInf));
        }

        // =====================================================================
        // SECTION 2: Quest markers -- only when alive (no corpse, not dead)
        // =====================================================================
        if (m_markers.empty() || !m_settings.enabled || !m_settings.showMarker || m_corpseActive || m_deadState) {
            m_markerValid = m_corpseNdcValid;
            saved.Restore();
            return;
        }

        int visibleCount = 0;
        for (const auto& mk : m_markers) {
            if (!mk.active) continue;
            float dx = mk.x - plrPos[0], dy = mk.y - plrPos[1], dz = mk.z - plrPos[2];
            if (dx*dx + dy*dy + dz*dz <= kMaxDistSq) ++visibleCount;
        }
        if (visibleCount == 0) {
            m_markerValid = m_corpseNdcValid;
            saved.Restore();
            return;
        }

        constexpr int kVertsPerMk = 6;
        const int neededVerts = visibleCount * kVertsPerMk;

        if (!m_vb || m_vbSize < neededVerts) {
            if (m_vb) { static_cast<IDirect3DVertexBuffer9*>(m_vb)->Release(); m_vb = nullptr; }
            int newSize = neededVerts + 32;
            IDirect3DVertexBuffer9* vb = nullptr;
            if (FAILED(rawDev->CreateVertexBuffer(
                newSize * sizeof(MarkerVert),
                D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
                D3DFVF_MARKERVERTS,
                D3DPOOL_DEFAULT, &vb, nullptr)) || !vb) {
                m_markerValid = m_corpseNdcValid;
                saved.Restore();
                return;
            }
            m_vb = vb;
            m_vbSize = newSize;
        }

        MarkerVert* verts = nullptr;
        if (FAILED(static_cast<IDirect3DVertexBuffer9*>(m_vb)->Lock(
            0, 0, (void**)&verts, D3DLOCK_DISCARD)) || !verts) {
            m_markerValid = m_corpseNdcValid;
            saved.Restore();
            return;
        }

        uint32_t markerColor = (m_alpha << 24) | 0xFFFFFF;
        int vi = 0;
        for (const auto& mk : m_markers) {
            if (!mk.active) continue;
            float pdx = mk.x - plrPos[0], pdy = mk.y - plrPos[1], pdz = mk.z - plrPos[2];
            if (pdx*pdx + pdy*pdy + pdz*pdz > kMaxDistSq) continue;
            constexpr float kIconAspect = 28.0f / 35.0f;
            verts[vi]   = MarkerVert{ -kIconAspect,  1.0f, 0, markerColor, 0.4395f, 0.002f    };
            verts[vi+1] = MarkerVert{ -kIconAspect, -1.0f, 0, markerColor, 0.4395f, 0.543875f };
            verts[vi+2] = MarkerVert{  kIconAspect,  1.0f, 0, markerColor, 0.8725f, 0.002f    };
            verts[vi+3] = MarkerVert{  kIconAspect,  1.0f, 0, markerColor, 0.8725f, 0.002f    };
            verts[vi+4] = MarkerVert{ -kIconAspect, -1.0f, 0, markerColor, 0.4395f, 0.543875f };
            verts[vi+5] = MarkerVert{  kIconAspect, -1.0f, 0, markerColor, 0.8725f, 0.543875f };
            vi += kVertsPerMk;
        }

        static_cast<IDirect3DVertexBuffer9*>(m_vb)->Unlock();
        rawDev->SetStreamSource(0, static_cast<IDirect3DVertexBuffer9*>(m_vb), 0, sizeof(MarkerVert));
        rawDev->SetFVF(D3DFVF_MARKERVERTS);

        int baseVertex = 0;
        bool ndcStored = false;
        bool navigationCandidate = false;
        bool drawOffscreenIndicator = false;
        float offscreenDirX = 0.0f;
        float offscreenDirY = 1.0f;

        for (const auto& mk : m_markers) {
            if (!mk.active) { baseVertex += kVertsPerMk; continue; }
            float pdx = mk.x - plrPos[0], pdy = mk.y - plrPos[1], pdz = mk.z - plrPos[2];
            if (pdx*pdx + pdy*pdy + pdz*pdz > kMaxDistSq) { baseVertex += kVertsPerMk; continue; }

            float vx = mk.x*viewMat[0] + mk.y*viewMat[4] + mk.z*viewMat[8]  + viewMat[12];
            float vy = mk.x*viewMat[1] + mk.y*viewMat[5] + mk.z*viewMat[9]  + viewMat[13];
            float vz = mk.x*viewMat[2] + mk.y*viewMat[6] + mk.z*viewMat[10] + viewMat[14];
            float vw = mk.x*viewMat[3] + mk.y*viewMat[7] + mk.z*viewMat[11] + viewMat[15];
            float viewZ = vz;
            float px = vx*projMat[0] + vy*projMat[4] + vz*projMat[8]  + vw*projMat[12];
            float py = vx*projMat[1] + vy*projMat[5] + vz*projMat[9]  + vw*projMat[13];
            float pw = vx*projMat[3] + vy*projMat[7] + vz*projMat[11] + vw*projMat[15];
            float ndcX = 0.0f, ndcY = 0.0f;
            if (pw >= 0.0001f) {
                ndcX = px / pw;
                ndcY = py / pw;
            } else {
                ndcX = (vx >= 0.0f) ? 2.0f : -2.0f;
                ndcY = (vy >= 0.0f) ? 2.0f : -2.0f;
            }

            // Only the selected quest is normally present, but keep this
            // deterministic if a stale marker survives briefly. The first valid
            // marker owns both the distance text and the off-screen indicator.
            if (!navigationCandidate) {
                navigationCandidate = true;
                m_dist = sqrtf(pdx*pdx + pdy*pdy + pdz*pdz);
                const bool onScreen = viewZ >= 0.1f && pw >= 0.0001f &&
                    ndcX >= -0.94f && ndcX <= 0.94f &&
                    ndcY >= -0.90f && ndcY <= 0.90f;
                if (onScreen) {
                    // Anchor FrameXML to the same projection the client uses for
                    // world text and target indicators. The hand-derived D3D
                    // projection above remains useful for drawing and bearings,
                    // but it does not include CGWorldFrame's viewport transform;
                    // using it for Lua made the distance label slide sideways as
                    // the camera moved.
                    float screenX = 0.0f, screenY = 0.0f;
                    float ddcWidth = 0.0f, ddcHeight = 0.0f;
                    bool projectedVisible = false;
                    const float markerPosition[3] = { mk.x, mk.y, mk.z };
                    if (ProjectToUi(markerPosition, screenX, screenY,
                                           projectedVisible, ddcWidth, ddcHeight) &&
                        projectedVisible) {
                        const float projectedNdcX = 2.0f * (screenX / ddcWidth) - 1.0f;
                        const float projectedNdcY = 2.0f * (screenY / ddcHeight) - 1.0f;
                        if (std::isfinite(projectedNdcX) && std::isfinite(projectedNdcY) &&
                            projectedNdcX >= -1.0f && projectedNdcX <= 1.0f &&
                            projectedNdcY >= -1.0f && projectedNdcY <= 1.0f) {
                            m_ndcX = projectedNdcX;
                            m_ndcY = projectedNdcY;
                            ndcStored = true;
                        }
                    }
                } else {
                    drawOffscreenIndicator = m_settings.showOffscreen;
                    // A ground-plane camera bearing keeps the arrow stable when
                    // the camera pitches: forward is up, behind is down.
                    offscreenDirX = vx;
                    offscreenDirY = vz;
                    const float dirLen = sqrtf(offscreenDirX*offscreenDirX +
                                               offscreenDirY*offscreenDirY);
                    if (dirLen > 0.0001f) {
                        offscreenDirX /= dirLen;
                        offscreenDirY /= dirLen;
                    } else {
                        offscreenDirX = 0.0f;
                        offscreenDirY = -1.0f;
                    }
                }
            }

            if (viewZ < 0.1f) { baseVertex += kVertsPerMk; continue; }
            float s = kMarkerPixelSize * m_settings.worldMarkerScale *
                2.0f * viewZ / (yScale * vpHeight);
            D3DMATRIX w = {};
            w._11 = rightX * s;  w._12 = rightY * s;  w._13 = rightZ * s;
            w._21 = upX * s;     w._22 = upY * s;     w._23 = upZ * s;
            w._31 = 0.0f;        w._32 = 0.0f;        w._33 = 0.0f;
            w._41 = mk.x;        w._42 = mk.y;        w._43 = mk.z;        w._44 = 1.0f;
            rawDev->SetTransform(D3DTS_WORLD, &w);
            rawDev->DrawPrimitive(D3DPT_TRIANGLELIST, baseVertex, 2);
            baseVertex += kVertsPerMk;
        }

        // When the destination is outside the viewport, replace the clipped
        // world marker with a compact navigation indicator above the player.
        // The gold diamond identifies the tracked quest; the atlas arrow below
        // it rotates toward the destination. It is deliberately not drawn while
        // the world-space marker is visible.
        if (drawOffscreenIndicator)
        {
            const float anchorX = plrPos[0];
            const float anchorY = plrPos[1];
            const float anchorZ = plrPos[2] + 2.8f;
            const float avx = anchorX*viewMat[0] + anchorY*viewMat[4] + anchorZ*viewMat[8]  + viewMat[12];
            const float avy = anchorX*viewMat[1] + anchorY*viewMat[5] + anchorZ*viewMat[9]  + viewMat[13];
            const float avz = anchorX*viewMat[2] + anchorY*viewMat[6] + anchorZ*viewMat[10] + viewMat[14];
            const float avw = anchorX*viewMat[3] + anchorY*viewMat[7] + anchorZ*viewMat[11] + viewMat[15];
            const float apx = avx*projMat[0] + avy*projMat[4] + avz*projMat[8]  + avw*projMat[12];
            const float apy = avx*projMat[1] + avy*projMat[5] + avz*projMat[9]  + avw*projMat[13];
            const float apw = avx*projMat[3] + avy*projMat[7] + avz*projMat[11] + avw*projMat[15];

            float cx = 0.0f;
            float cy = -0.10f;
            if (apw >= 0.0001f) {
                cx = std::clamp(apx / apw, -0.80f, 0.80f);
                cy = std::clamp(apy / apw + 0.055f, -0.70f, 0.72f);
            }

            const float vpWidth = static_cast<float>(vp.Width);
            cx = std::clamp(cx + (2.0f * m_settings.offscreenOffsetX / vpWidth),
                            -0.90f, 0.90f);
            cy = std::clamp(cy + (2.0f * m_settings.offscreenOffsetY / vpHeight),
                            -0.82f, 0.82f);

            D3DMATRIX id = {};
            id._11 = id._22 = id._33 = id._44 = 1.0f;
            rawDev->SetTransform(D3DTS_WORLD, &id);
            rawDev->SetTransform(D3DTS_VIEW, &id);
            rawDev->SetTransform(D3DTS_PROJECTION, &id);
            rawDev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            rawDev->SetFVF(D3DFVF_MARKERVERTS);

            auto fillScreenQuad = [](MarkerVert (&v)[6], float qx, float qy,
                                     float hw, float hh, uint32_t color,
                                     float u0, float v0, float u1, float v1) {
                const float x0 = qx - hw, x1 = qx + hw;
                const float y0 = qy - hh, y1 = qy + hh;
                v[0] = MarkerVert{ x0, y1, 0.5f, color, u0, v0 };
                v[1] = MarkerVert{ x0, y0, 0.5f, color, u0, v1 };
                v[2] = MarkerVert{ x1, y1, 0.5f, color, u1, v0 };
                v[3] = MarkerVert{ x1, y1, 0.5f, color, u1, v0 };
                v[4] = MarkerVert{ x0, y0, 0.5f, color, u0, v1 };
                v[5] = MarkerVert{ x1, y0, 0.5f, color, u1, v1 };
            };

            const uint32_t indicatorColor = (m_alpha << 24) | 0xFFFFFF;
            const float indicatorScale = m_settings.offscreenScale;
            const float diamondHalfW = 20.0f * indicatorScale / vpWidth;
            const float diamondHalfH = 25.0f * indicatorScale / vpHeight;
            MarkerVert diamond[6];
            fillScreenQuad(diamond, cx, cy, diamondHalfW, diamondHalfH,
                           indicatorColor, 0.4395f, 0.002f, 0.8725f, 0.543875f);
            rawDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, diamond, sizeof(MarkerVert));

            const float arrowHalfW = 14.0f * indicatorScale / vpWidth;
            const float arrowHalfH = 11.0f * indicatorScale / vpHeight;
            const float arrowCx = cx;
            const float arrowCy = cy - diamondHalfH - arrowHalfH -
                (4.0f * indicatorScale / vpHeight);
            const float rx = offscreenDirY * arrowHalfW;
            const float ry = -offscreenDirX * arrowHalfW;
            const float ux = offscreenDirX * arrowHalfH;
            const float uy = offscreenDirY * arrowHalfH;
            const float lx = arrowCx - rx, ly = arrowCy - ry;
            const float hx = arrowCx + rx, hy = arrowCy + ry;
            MarkerVert arrow[6];
            arrow[0] = MarkerVert{ lx + ux, ly + uy, 0.5f, indicatorColor, 0.002f, 0.543875f };
            arrow[1] = MarkerVert{ lx - ux, ly - uy, 0.5f, indicatorColor, 0.002f, 0.8725f };
            arrow[2] = MarkerVert{ hx + ux, hy + uy, 0.5f, indicatorColor, 0.4355f, 0.543875f };
            arrow[3] = MarkerVert{ hx + ux, hy + uy, 0.5f, indicatorColor, 0.4355f, 0.543875f };
            arrow[4] = MarkerVert{ lx - ux, ly - uy, 0.5f, indicatorColor, 0.002f, 0.8725f };
            arrow[5] = MarkerVert{ hx - ux, hy - uy, 0.5f, indicatorColor, 0.4355f, 0.8725f };
            rawDev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, arrow, sizeof(MarkerVert));
        }

        m_markerValid = ndcStored;
        saved.Restore();
    }

    bool InstallQuestMarker()
    {
        if (!g_instance)
            g_instance = new QuestMarker();

        bool ok = true;
        ok &= Network()->RegisterClientOpcode(
            kCmsgQuestMarkerRequest, "CMSG_WXL_QUEST_TRACKER_REQUEST") != 0;
        ok &= Network()->RegisterServerOpcode(
            kSmsgQuestMarkerUpdate, "SMSG_WXL_QUEST_TRACKER_MARKER",
            &OnMarkerPacket, nullptr) != 0;
        ok &= Network()->RegisterServerOpcode(
            kSmsgQuestKillEntries, "SMSG_WXL_QUEST_TRACKER_KILL_ENTRIES",
            &OnKillEntriesPacket, nullptr) != 0;
        ok &= Network()->RegisterServerOpcode(
            kSmsgQuestCorpsePosition, "SMSG_WXL_QUEST_TRACKER_CORPSE",
            &OnCorpsePacket, nullptr) != 0;

        ok &= FrameScript()->RegisterFunction("SetQuestMarker", &QuestMarker::ScriptSetMarker) != 0;
        ok &= FrameScript()->RegisterFunction("ClearAllQuestMarkers", &QuestMarker::ScriptClearAll) != 0;
        ok &= FrameScript()->RegisterFunction("GetDistInfo", &QuestMarker::ScriptGetDistInfo) != 0;
        ok &= FrameScript()->RegisterFunction("SetMarkerAlpha", &QuestMarker::ScriptSetAlpha) != 0;
        ok &= FrameScript()->RegisterFunction("QuestTrackerReloadSettings", &QuestMarker::ScriptReloadSettings) != 0;
        ok &= FrameScript()->RegisterFunction("SetCorpseMarker", &QuestMarker::ScriptSetCorpseMarker) != 0;
        ok &= FrameScript()->RegisterFunction("ClearCorpseMarker", &QuestMarker::ScriptClearCorpseMarker) != 0;
        ok &= FrameScript()->RegisterFunction("IsCorpseActive", &QuestMarker::ScriptIsCorpseActive) != 0;
        ok &= FrameScript()->RegisterFunction("GetCorpseDistInfo", &QuestMarker::ScriptGetCorpseDistInfo) != 0;
        ok &= FrameScript()->RegisterFunction("SetDeadState", &QuestMarker::ScriptSetDeadState) != 0;
        ok &= FrameScript()->RegisterFunction("GetPlayerPosition", &QuestMarker::ScriptGetPlayerPos) != 0;
        ok &= FrameScript()->RegisterFunction("SetTrackerDiamond", &QuestMarker::ScriptSetTrackerDiamond) != 0;
        ok &= FrameScript()->RegisterFunction("_WXL_QUEST_TRACKER_REQUEST", &ScriptRequest) != 0;
        ok &= FrameScript()->RegisterFunction("_WXL_QUEST_TRACKER_POP_MARKER", &ScriptPopMarker) != 0;
        ok &= FrameScript()->RegisterFunction("_WXL_QUEST_TRACKER_POP_KILL", &ScriptPopKillEntries) != 0;
        ok &= FrameScript()->RegisterFunction("_WXL_QUEST_TRACKER_POP_CORPSE", &ScriptPopCorpse) != 0;

        constexpr char bootstrap[] = R"lua(
do
    wxlwow = wxlwow or {}
    wxlwow.quest_tracker = wxlwow.quest_tracker or {}
    local tracker = wxlwow.quest_tracker
    tracker.Request = _WXL_QUEST_TRACKER_REQUEST
    tracker.PopMarker = _WXL_QUEST_TRACKER_POP_MARKER
    tracker.PopKillEntries = _WXL_QUEST_TRACKER_POP_KILL
    tracker.PopCorpse = _WXL_QUEST_TRACKER_POP_CORPSE
    _WXL_QUEST_TRACKER_REQUEST = nil
    _WXL_QUEST_TRACKER_POP_MARKER = nil
    _WXL_QUEST_TRACKER_POP_KILL = nil
    _WXL_QUEST_TRACKER_POP_CORPSE = nil

    local loader = CreateFrame("Frame")
    loader:RegisterEvent("PLAYER_LOGIN")
    loader:RegisterEvent("PLAYER_ENTERING_WORLD")
    loader:SetScript("OnEvent", function(self)
        if EnableAddOn then EnableAddOn("QuestMarker") end
        if (not IsAddOnLoaded or not IsAddOnLoaded("QuestMarker")) and LoadAddOn then
            LoadAddOn("QuestMarker")
        end
        if IsAddOnLoaded and IsAddOnLoaded("QuestMarker") then
            self:UnregisterAllEvents()
        end
    end)
end
)lua";
        ok &= FrameScript()->RegisterScript("quest-tracker-bridge", bootstrap) != 0;
        ok &= InstallQuestHighlight();

        if (ok)
            WLOG_INFO("WXL-UI-Tracker v2 renderer, transport, and objective highlights installed");
        return ok;
    }
}
