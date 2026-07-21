/*
 * WarcraftXL quest markers for AzerothCore.
 *
 * CMSG 0x51F requests a snapshot; SMSG 0x102 returns world-space markers.
 * Both use the stock packet framing and dispatcher lifecycle.
 */

#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
    constexpr uint16 WXL_CMSG_QUEST_MARKER_REQUEST = 0x051F;
    constexpr uint16 WXL_SMSG_QUEST_MARKER_UPDATE = 0x0102;

    struct QuestMarkerPosition
    {
        uint32 QuestId = 0;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        uint32 Type = 0;
    };

    class WxlQuestMarkerService
    {
    public:
        static WxlQuestMarkerService& Instance()
        {
            static WxlQuestMarkerService instance;
            return instance;
        }

        static void HandleRequest(WorldSession* session, WorldPacket& packet)
        {
            if (!session || !packet.empty())
                return;
            Instance().SendSnapshot(session->GetPlayer());
        }

        void SendSnapshot(Player* player)
        {
            if (!player || !player->IsInWorld() || !player->GetSession())
                return;

            std::vector<QuestMarkerPosition> markers;
            markers.reserve(MAX_QUEST_LOG_SIZE);

            for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 const questId = player->GetQuestSlotQuestId(slot);
                if (!questId)
                    continue;

                QuestStatus const status = player->GetQuestStatus(questId);
                if (status != QUEST_STATUS_INCOMPLETE && status != QUEST_STATUS_COMPLETE)
                    continue;

                QuestMarkerPosition marker;
                if (FindMarker(player, questId, status == QUEST_STATUS_COMPLETE, marker))
                    markers.push_back(marker);
            }

            WorldSession* session = player->GetSession();
            std::unordered_set<uint32> current;
            current.reserve(markers.size());
            for (QuestMarkerPosition const& marker : markers)
                current.insert(marker.QuestId);

            std::vector<uint32> removed;
            {
                std::lock_guard<std::mutex> lock(_stateMutex);
                std::unordered_set<uint32>& previous = _sentQuestIds[session];
                for (uint32 questId : previous)
                    if (current.find(questId) == current.end())
                        removed.push_back(questId);
                previous = current;
            }

            for (uint32 questId : removed)
                SendMarker(session, QuestMarkerPosition{questId}, false);

            for (QuestMarkerPosition const& marker : markers)
                SendMarker(session, marker, true);
        }

        void ClearSession(WorldSession* session)
        {
            if (!session)
                return;

            std::lock_guard<std::mutex> lock(_stateMutex);
            _sentQuestIds.erase(session);
        }

    private:
        static bool FindMarker(Player* player, uint32 questId, bool turnIn,
            QuestMarkerPosition& result)
        {
            QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(questId);
            if (!pois || pois->empty())
                return false;

            struct Candidate
            {
                double X = 0.0;
                double Y = 0.0;
                double DistanceSq = (std::numeric_limits<double>::max)();
                bool Preferred = false;
            } best;

            bool found = false;
            for (QuestPOI const& poi : *pois)
            {
                if (poi.MapId != player->GetMapId() || poi.points.empty())
                    continue;

                double x = 0.0;
                double y = 0.0;
                for (QuestPOIPoint const& point : poi.points)
                {
                    x += point.x;
                    y += point.y;
                }
                x /= static_cast<double>(poi.points.size());
                y /= static_cast<double>(poi.points.size());

                bool const preferred = turnIn ? poi.ObjectiveIndex < 0 : poi.ObjectiveIndex >= 0;
                double const dx = x - player->GetPositionX();
                double const dy = y - player->GetPositionY();
                double const distanceSq = dx * dx + dy * dy;

                if (!found || (preferred && !best.Preferred) ||
                    (preferred == best.Preferred && distanceSq < best.DistanceSq))
                {
                    best = {x, y, distanceSq, preferred};
                    found = true;
                }
            }

            if (!found)
                return false;

            float z = player->GetMap()->GetHeight(
                static_cast<float>(best.X), static_cast<float>(best.Y), MAX_HEIGHT, false);
            if (z <= INVALID_HEIGHT)
                z = player->GetPositionZ();

            result.QuestId = questId;
            result.X = best.X;
            result.Y = best.Y;
            result.Z = static_cast<double>(z + 2.5f);
            result.Type = turnIn ? 1u : 0u;
            return true;
        }

        static void SendMarker(WorldSession* session, QuestMarkerPosition const& marker, bool active)
        {
            WorldPacket packet(WXL_SMSG_QUEST_MARKER_UPDATE, 36);
            packet << uint32(marker.QuestId);
            packet << uint32(active ? 1 : 0);
            packet << double(marker.X);
            packet << double(marker.Y);
            packet << double(marker.Z);
            packet << uint32(marker.Type);
            session->SendPacket(&packet);
        }

        std::mutex _stateMutex;
        std::unordered_map<WorldSession*, std::unordered_set<uint32>> _sentQuestIds;
    };

    class wxl_quest_marker_player_script final : public PlayerScript
    {
    public:
        wxl_quest_marker_player_script()
            : PlayerScript("wxl_quest_marker_player_script",
                {PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_LOGOUT,
                 PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_QUEST_ABANDON})
        {
        }

        void OnPlayerCompleteQuest(Player* player, Quest const*) override
        {
            WxlQuestMarkerService::Instance().SendSnapshot(player);
        }

        void OnPlayerMapChanged(Player* player) override
        {
            WxlQuestMarkerService::Instance().SendSnapshot(player);
        }

        void OnPlayerQuestAbandon(Player* player, uint32) override
        {
            WxlQuestMarkerService::Instance().SendSnapshot(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            WxlQuestMarkerService::Instance().ClearSession(player ? player->GetSession() : nullptr);
        }
    };

    class wxl_quest_marker_server_script final : public ServerScript
    {
    public:
        wxl_quest_marker_server_script()
            : ServerScript("wxl_quest_marker_server_script", {SERVERHOOK_CAN_PACKET_RECEIVE}) { }

        bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
        {
            if (packet.GetOpcode() != WXL_CMSG_QUEST_MARKER_REQUEST)
                return true;
            WxlQuestMarkerService::HandleRequest(session, packet);
            return false;
        }
    };
}

void AddSC_wxl_quest_marker()
{
    new wxl_quest_marker_server_script();
    new wxl_quest_marker_player_script();
}
