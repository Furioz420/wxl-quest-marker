/*
 * WarcraftXL port of bozo-1/WXL-UI-Tracker for AzerothCore.
 *
 * The client selects a quest; the server validates its active status and owns
 * every world coordinate. Native WXL opcodes replace TSWoW's custom-packet
 * multiplexing while preserving the upstream logical request types:
 *   101 exact quest, 102 full snapshot, 103 objective creature entries.
 */

#include "Corpse.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr uint16 WXL_CMSG_QUEST_TRACKER_REQUEST = 0x051F;
    constexpr uint16 WXL_SMSG_QUEST_TRACKER_MARKER = 0x0520;
    constexpr uint16 WXL_SMSG_QUEST_TRACKER_KILL_ENTRIES = 0x0536;
    constexpr uint16 WXL_SMSG_QUEST_TRACKER_CORPSE = 0x0537;

    constexpr uint16 WXL_REQUEST_EXACT_QUEST = 101;
    constexpr uint16 WXL_REQUEST_SNAPSHOT = 102;
    constexpr uint16 WXL_REQUEST_KILL_ENTRIES = 103;

    struct QuestMarkerPosition
    {
        uint32 QuestId = 0;
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
        uint32 Type = 0; // 0 objective, 1 turn-in
    };

    struct SpawnPosition
    {
        double X = 0.0;
        double Y = 0.0;
        double Z = 0.0;
    };

    void AppendUnique(std::vector<uint32>& entries, uint32 entry)
    {
        if (entry && entries.size() < 4 &&
            std::find(entries.begin(), entries.end(), entry) == entries.end())
            entries.push_back(entry);
    }

    class WxlQuestTrackerService
    {
    public:
        static WxlQuestTrackerService& Instance()
        {
            static WxlQuestTrackerService instance;
            return instance;
        }

        static void HandleRequest(WorldSession* session, WorldPacket& packet)
        {
            if (!session || !session->GetPlayer() || packet.size() < sizeof(uint16))
                return;

            uint16 request = 0;
            packet >> request;
            uint32 questId = 0;
            if (request == WXL_REQUEST_EXACT_QUEST || request == WXL_REQUEST_KILL_ENTRIES)
            {
                if (packet.size() - packet.rpos() != sizeof(uint32))
                {
                    LOG_WARN("module.wxl.questtracker",
                        "Rejected malformed quest tracker request {} ({} trailing bytes).",
                        request, packet.size() - packet.rpos());
                    return;
                }
                packet >> questId;
            }
            else if (request != WXL_REQUEST_SNAPSHOT || packet.rpos() != packet.size())
            {
                LOG_WARN("module.wxl.questtracker",
                    "Rejected unknown or malformed quest tracker request {}.", request);
                return;
            }

            Player* player = session->GetPlayer();
            switch (request)
            {
                case WXL_REQUEST_EXACT_QUEST:
                    Instance().SendExactQuest(player, questId);
                    Instance().SendKillEntries(player, questId);
                    break;
                case WXL_REQUEST_SNAPSHOT:
                    Instance().SendSnapshot(player);
                    break;
                case WXL_REQUEST_KILL_ENTRIES:
                    Instance().SendKillEntries(player, questId);
                    break;
                default:
                    break;
            }
        }

        void SendSnapshot(Player* player)
        {
            if (!CanSend(player))
                return;

            SendCorpse(player);
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
                if (FindMarker(player, questId, status, marker))
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
                    if (!current.contains(questId))
                        removed.push_back(questId);
                previous = current;
            }

            for (uint32 questId : removed)
                SendMarker(session, QuestMarkerPosition{questId}, false);
            for (QuestMarkerPosition const& marker : markers)
                SendMarker(session, marker, true);

            // Upstream batch-complete sentinel. It lets the addon settle its
            // selection only after every active quest has arrived.
            SendMarker(session, QuestMarkerPosition{}, false);
        }

        void SendExactQuest(Player* player, uint32 questId)
        {
            if (!CanSend(player) || !questId)
                return;

            QuestStatus const status = player->GetQuestStatus(questId);
            QuestMarkerPosition marker{questId};
            bool const active =
                (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE) &&
                FindMarker(player, questId, status, marker);
            SendMarker(player->GetSession(), marker, active);

            std::lock_guard<std::mutex> lock(_stateMutex);
            std::unordered_set<uint32>& sent = _sentQuestIds[player->GetSession()];
            if (active)
                sent.insert(questId);
            else
                sent.erase(questId);
        }

        void SendKillEntries(Player* player, uint32 questId, bool forceTurnIn = false)
        {
            if (!CanSend(player))
                return;

            std::vector<uint32> entries;
            QuestStatus const status = questId ? player->GetQuestStatus(questId) : QUEST_STATUS_NONE;
            if (forceTurnIn || status == QUEST_STATUS_COMPLETE)
                entries = GetTurnInEntries(questId);
            else if (status == QUEST_STATUS_INCOMPLETE)
                entries = GetKillEntries(questId);

            WorldPacket packet(WXL_SMSG_QUEST_TRACKER_KILL_ENTRIES,
                4 + entries.size() * sizeof(uint32));
            packet << uint32(entries.size());
            for (uint32 entry : entries)
                packet << entry;
            player->GetSession()->SendPacket(&packet);
        }

        void SendCorpse(Player* player)
        {
            if (!CanSend(player))
                return;

            Corpse* corpse = player->GetCorpse();
            bool const active = corpse && corpse->GetMapId() == player->GetMapId();
            WorldPacket packet(WXL_SMSG_QUEST_TRACKER_CORPSE, active ? 28 : 4);
            packet << uint32(active ? 1 : 0);
            if (active)
            {
                packet << double(corpse->GetPositionX());
                packet << double(corpse->GetPositionY());
                packet << double(corpse->GetPositionZ() + 2.5f);
            }
            player->GetSession()->SendPacket(&packet);
        }

        void ClearSession(WorldSession* session)
        {
            if (!session)
                return;
            std::lock_guard<std::mutex> lock(_stateMutex);
            _sentQuestIds.erase(session);
        }

    private:
        static bool CanSend(Player* player)
        {
            return player && player->IsInWorld() && player->GetSession();
        }

        static std::vector<uint32> GetKillEntries(uint32 questId)
        {
            std::vector<uint32> entries;
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                return entries;

            for (int32 value : quest->RequiredNpcOrGo)
                if (value > 0)
                    AppendUnique(entries, uint32(value));

            auto appendQuery = [&entries](QueryResult result)
            {
                if (!result)
                    return;
                do
                {
                    AppendUnique(entries, result->Fetch()[0].Get<uint32>());
                } while (entries.size() < 4 && result->NextRow());
            };

            if (entries.size() < 4)
                appendQuery(WorldDatabase.Query(
                    "SELECT DISTINCT clt.`Entry` FROM `creature_loot_template` clt "
                    "JOIN `quest_template` qt ON (clt.`Item` = qt.`ItemDrop1` OR "
                    "clt.`Item` = qt.`ItemDrop2` OR clt.`Item` = qt.`ItemDrop3` OR "
                    "clt.`Item` = qt.`ItemDrop4`) WHERE qt.`ID` = {} AND "
                    "clt.`Item` > 0 LIMIT 4", questId));

            if (entries.size() < 4)
                appendQuery(WorldDatabase.Query(
                    "SELECT DISTINCT clt.`Entry` FROM `creature_loot_template` clt "
                    "JOIN `quest_template` qt ON (clt.`Item` = qt.`RequiredItemId1` OR "
                    "clt.`Item` = qt.`RequiredItemId2` OR clt.`Item` = qt.`RequiredItemId3` OR "
                    "clt.`Item` = qt.`RequiredItemId4` OR clt.`Item` = qt.`RequiredItemId5` OR "
                    "clt.`Item` = qt.`RequiredItemId6`) WHERE qt.`ID` = {} AND "
                    "clt.`Item` > 0 LIMIT 4", questId));
            return entries;
        }

        static std::vector<uint32> GetTurnInEntries(uint32 questId)
        {
            std::vector<uint32> entries;
            QueryResult result = WorldDatabase.Query(
                "SELECT DISTINCT `id` FROM `creature_questender` "
                "WHERE `quest` = {} LIMIT 4", questId);
            if (!result)
                return entries;

            do
            {
                AppendUnique(entries, result->Fetch()[0].Get<uint32>());
            } while (entries.size() < 4 && result->NextRow());
            return entries;
        }

        static std::optional<SpawnPosition> QueryCreatureSpawn(Player* player, uint32 entry)
        {
            QueryResult result = WorldDatabase.Query(
                "SELECT `position_x`, `position_y`, `position_z` FROM `creature` "
                "WHERE `id1` = {} AND `map` = {} ORDER BY "
                "POW(`position_x` - {}, 2) + POW(`position_y` - {}, 2) LIMIT 1",
                entry, player->GetMapId(), player->GetPositionX(), player->GetPositionY());
            if (!result)
                return std::nullopt;
            Field* fields = result->Fetch();
            return SpawnPosition{fields[0].Get<double>(), fields[1].Get<double>(),
                fields[2].Get<double>()};
        }

        static std::optional<SpawnPosition> QueryGameObjectSpawn(Player* player, uint32 entry)
        {
            QueryResult result = WorldDatabase.Query(
                "SELECT `position_x`, `position_y`, `position_z` FROM `gameobject` "
                "WHERE `id` = {} AND `map` = {} ORDER BY "
                "POW(`position_x` - {}, 2) + POW(`position_y` - {}, 2) LIMIT 1",
                entry, player->GetMapId(), player->GetPositionX(), player->GetPositionY());
            if (!result)
                return std::nullopt;
            Field* fields = result->Fetch();
            return SpawnPosition{fields[0].Get<double>(), fields[1].Get<double>(),
                fields[2].Get<double>()};
        }

        static bool FindRelationSpawn(Player* player, uint32 questId,
            char const* relationTable, bool creature, SpawnPosition& result)
        {
            QueryResult query;
            if (creature)
                query = WorldDatabase.Query(
                    "SELECT c.`position_x`, c.`position_y`, c.`position_z` FROM `{}` r "
                    "JOIN `creature` c ON c.`id1` = r.`id` WHERE r.`quest` = {} "
                    "AND c.`map` = {} ORDER BY POW(c.`position_x` - {}, 2) + "
                    "POW(c.`position_y` - {}, 2) LIMIT 1", relationTable, questId,
                    player->GetMapId(), player->GetPositionX(), player->GetPositionY());
            else
                query = WorldDatabase.Query(
                    "SELECT g.`position_x`, g.`position_y`, g.`position_z` FROM `{}` r "
                    "JOIN `gameobject` g ON g.`id` = r.`id` WHERE r.`quest` = {} "
                    "AND g.`map` = {} ORDER BY POW(g.`position_x` - {}, 2) + "
                    "POW(g.`position_y` - {}, 2) LIMIT 1", relationTable, questId,
                    player->GetMapId(), player->GetPositionX(), player->GetPositionY());
            if (!query)
                return false;
            Field* fields = query->Fetch();
            result = {fields[0].Get<double>(), fields[1].Get<double>(), fields[2].Get<double>()};
            return true;
        }

        static int32 FirstIncompleteObjective(Player* player, Quest const* quest)
        {
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
            {
                int32 const entry = quest->RequiredNpcOrGo[i];
                if (entry && player->GetReqKillOrCastCurrentCount(quest->GetQuestId(), entry) <
                    quest->RequiredNpcOrGoCount[i])
                    return i;
            }
            return -1;
        }

        static bool FindPoi(Player* player, Quest const* quest, QuestStatus status,
            QuestMarkerPosition& marker)
        {
            QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(quest->GetQuestId());
            if (!pois || pois->empty())
                return false;

            int32 const incompleteObjective =
                status == QUEST_STATUS_INCOMPLETE ? FirstIncompleteObjective(player, quest) : -1;
            struct Candidate
            {
                double X = 0.0;
                double Y = 0.0;
                double DistanceSq = (std::numeric_limits<double>::max)();
                uint8 Priority = 0;
            } best;
            bool found = false;

            for (QuestPOI const& poi : *pois)
            {
                if (poi.MapId != player->GetMapId() || poi.points.empty())
                    continue;
                bool const turnIn = status == QUEST_STATUS_COMPLETE;
                if (turnIn && poi.ObjectiveIndex >= 0)
                    continue;
                if (!turnIn && poi.ObjectiveIndex < 0)
                    continue;

                double x = 0.0;
                double y = 0.0;
                for (QuestPOIPoint const& point : poi.points)
                {
                    x += point.x;
                    y += point.y;
                }
                x /= poi.points.size();
                y /= poi.points.size();
                uint8 const priority = turnIn ? 2 :
                    (incompleteObjective >= 0 && poi.ObjectiveIndex == incompleteObjective ? 2 : 1);
                double const dx = x - player->GetPositionX();
                double const dy = y - player->GetPositionY();
                double const distanceSq = dx * dx + dy * dy;
                if (!found || priority > best.Priority ||
                    (priority == best.Priority && distanceSq < best.DistanceSq))
                {
                    best = {x, y, distanceSq, priority};
                    found = true;
                }
            }
            if (!found)
                return false;

            float z = player->GetMap()->GetHeight(float(best.X), float(best.Y), MAX_HEIGHT, false);
            if (z <= INVALID_HEIGHT)
                z = player->GetPositionZ();
            marker = {quest->GetQuestId(), best.X, best.Y, double(z + 3.0f),
                status == QUEST_STATUS_COMPLETE ? 1u : 0u};
            return true;
        }

        static bool FindTurnInFallback(Player* player, uint32 questId, QuestMarkerPosition& marker)
        {
            SpawnPosition pos;
            bool found = FindRelationSpawn(player, questId, "creature_questender", true, pos) ||
                FindRelationSpawn(player, questId, "gameobject_questender", false, pos) ||
                FindRelationSpawn(player, questId, "creature_queststarter", true, pos) ||
                FindRelationSpawn(player, questId, "gameobject_queststarter", false, pos);
            if (!found)
                return false;
            marker = {questId, pos.X, pos.Y, pos.Z + 3.0, 1};
            return true;
        }

        static bool FindObjectiveFallback(Player* player, Quest const* quest,
            QuestMarkerPosition& marker)
        {
            int32 const preferred = FirstIncompleteObjective(player, quest);
            std::array<uint8, QUEST_OBJECTIVES_COUNT> order{};
            uint8 count = 0;
            if (preferred >= 0)
                order[count++] = uint8(preferred);
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (i != preferred)
                    order[count++] = i;

            for (uint8 i : order)
            {
                int32 const value = quest->RequiredNpcOrGo[i];
                if (!value)
                    continue;
                std::optional<SpawnPosition> pos = value > 0
                    ? QueryCreatureSpawn(player, uint32(value))
                    : QueryGameObjectSpawn(player, uint32(-value));
                if (pos)
                {
                    marker = {quest->GetQuestId(), pos->X, pos->Y, pos->Z + 3.0, 0};
                    return true;
                }
            }

            for (uint32 entry : GetKillEntries(quest->GetQuestId()))
                if (std::optional<SpawnPosition> pos = QueryCreatureSpawn(player, entry))
                {
                    marker = {quest->GetQuestId(), pos->X, pos->Y, pos->Z + 3.0, 0};
                    return true;
                }
            return false;
        }

        static bool FindMarker(Player* player, uint32 questId, QuestStatus status,
            QuestMarkerPosition& marker)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
            if (!quest)
                return false;
            if (FindPoi(player, quest, status, marker))
                return true;
            return status == QUEST_STATUS_COMPLETE
                ? FindTurnInFallback(player, questId, marker)
                : FindObjectiveFallback(player, quest, marker);
        }

        static void SendMarker(WorldSession* session, QuestMarkerPosition const& marker, bool active)
        {
            if (!session)
                return;
            WorldPacket packet(WXL_SMSG_QUEST_TRACKER_MARKER, 36);
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

    class wxl_quest_tracker_player_script final : public PlayerScript
    {
    public:
        wxl_quest_tracker_player_script()
            : PlayerScript("wxl_quest_tracker_player_script",
                {PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_PLAYER_JUST_DIED,
                 PLAYERHOOK_ON_PLAYER_RELEASED_GHOST, PLAYERHOOK_ON_PLAYER_RESURRECT,
                 PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST, PLAYERHOOK_ON_LOGOUT,
                 PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_QUEST_ABANDON,
                 PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE}) { }

        void OnPlayerLogin(Player* player) override
        {
            WxlQuestTrackerService::Instance().SendCorpse(player);
        }

        void OnPlayerJustDied(Player* player) override
        {
            WxlQuestTrackerService::Instance().SendCorpse(player);
        }

        void OnPlayerReleasedGhost(Player* player) override
        {
            WxlQuestTrackerService::Instance().SendCorpse(player);
        }

        void OnPlayerResurrect(Player* player, float, bool) override
        {
            WxlQuestTrackerService::Instance().SendCorpse(player);
        }

        void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
        {
            WxlQuestTrackerService::Instance().SendSnapshot(player);
            if (quest)
                WxlQuestTrackerService::Instance().SendKillEntries(player, quest->GetQuestId());
        }

        bool OnPlayerBeforeQuestComplete(Player* player, uint32 questId) override
        {
            // CompleteQuest has not changed the status yet at this hook, so explicitly
            // swap the objective rings to the creature quest enders. The normal
            // OnPlayerCompleteQuest path later clears the list after the reward is taken.
            WxlQuestTrackerService::Instance().SendKillEntries(player, questId, true);
            return true;
        }

        void OnPlayerMapChanged(Player* player) override
        {
            WxlQuestTrackerService::Instance().SendSnapshot(player);
        }

        void OnPlayerQuestAbandon(Player* player, uint32) override
        {
            WxlQuestTrackerService::Instance().SendSnapshot(player);
        }

        void OnPlayerLogout(Player* player) override
        {
            WxlQuestTrackerService::Instance().ClearSession(
                player ? player->GetSession() : nullptr);
        }
    };

    class wxl_quest_tracker_server_script final : public ServerScript
    {
    public:
        wxl_quest_tracker_server_script()
            : ServerScript("wxl_quest_tracker_server_script", {SERVERHOOK_CAN_PACKET_RECEIVE}) { }

        bool CanPacketReceive(WorldSession* session, WorldPacket& packet) override
        {
            if (packet.GetOpcode() != WXL_CMSG_QUEST_TRACKER_REQUEST)
                return true;
            WxlQuestTrackerService::HandleRequest(session, packet);
            return false;
        }
    };
}

void AddSC_wxl_quest_marker()
{
    new wxl_quest_tracker_server_script();
    new wxl_quest_tracker_player_script();
}
