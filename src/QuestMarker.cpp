#include "QuestMarker.hpp"
#include "TextureLoader.hpp"

#include "core/Logger.hpp"
#include "game/gx/Gx.hpp"
#include "game/camera/Camera.hpp"
#include "game/world/World.hpp"
#include "game/io/Io.hpp"
#include "events/Event.hpp"
#include "events/EventScript.hpp"
#include "offsets/engine/Lua.hpp"
#include "runtime/LuaBindings.hpp"
#include "runtime/ModuleInstall.hpp"
#include "wxl-client-extensions/src/Network.hpp"
#include <d3d9.h>
#include <cstring>

#define D3DFVF_MARKERVERTS (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

namespace wxl::scripts::questmarker
{
    namespace gx    = wxl::game::gx;
    namespace cam   = wxl::game::camera;
    namespace world = wxl::game::world;

    namespace
    {
        constexpr float kMarkerPixelSize = 16.0f;
        constexpr float kMaxDistSq = 100000.0f * 100000.0f;

        QuestMarker g_instance;

        struct MarkerVert
        {
            float x, y, z;
            uint32_t color;
            float tu, tv;
        };

        constexpr int kTotalVerts = 6;

        class ScopedDeviceState final
        {
        public:
            explicit ScopedDeviceState(gx::Device9 dev)
            {
                auto* d = static_cast<IDirect3DDevice9*>(dev.raw());
                if (d && SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL, &state_)) && state_)
                    state_->Capture();
            }
            ~ScopedDeviceState() { Restore(); }
            void Restore()
            {
                if (!state_) return;
                state_->Apply();
                state_->Release();
                state_ = nullptr;
            }
        private:
            IDirect3DStateBlock9* state_ = nullptr;
        };

        using LuaNumFn = double(__cdecl*)(void* state, int idx);
    }

    int QuestMarker::ScriptSetMarker(void* state)
    {
        auto toNum = wxl::game::Native<LuaNumFn>(wxl::offsets::engine::lua::kLuaToNumber);
        const uint32_t questId    = static_cast<uint32_t>(toNum(state, 1));
        const bool     active     = toNum(state, 2) != 0.0;
        const double   x          = toNum(state, 3);
        const double   y          = toNum(state, 4);
        const double   z          = toNum(state, 5);
        const uint32_t markerType = static_cast<uint32_t>(toNum(state, 6));

        g_instance.SetMarker(questId, active, static_cast<float>(x),
                             static_cast<float>(y), static_cast<float>(z), markerType);
        return 0;
    }

    int QuestMarker::ScriptClearAll(void* state)
    {
        g_instance.ClearAllMarkers();
        return 0;
    }

    int QuestMarker::ScriptGetDistInfo(void* state)
    {
        auto pushNum = wxl::game::Native<wxl::offsets::engine::lua::LuaPushNumberFn>(wxl::offsets::engine::lua::kLuaPushNumber);
        auto pushNil = wxl::game::Native<wxl::offsets::engine::lua::LuaPushNilFn>(wxl::offsets::engine::lua::kLuaPushNil);
        if (!g_instance.m_markerValid) { pushNil(state); pushNil(state); pushNil(state); return 3; }
        pushNum(state, g_instance.m_ndcX);
        pushNum(state, g_instance.m_ndcY);
        pushNum(state, g_instance.m_dist);
        return 3;
    }

    int QuestMarker::ScriptSetAlpha(void* state)
    {
        auto toNum = wxl::game::Native<LuaNumFn>(wxl::offsets::engine::lua::kLuaToNumber);
        auto alpha = static_cast<uint32_t>(toNum(state, 1));
        if (alpha > 255) alpha = 255;
        g_instance.m_alpha = alpha;
        return 0;
    }

    static void InstallModule()
    {
        wxl::runtime::lua::RegisterCVar("wxlQuestMarkerEnabled", "1");
        wxl::runtime::lua::RegisterCVar("wxlQuestMarkerAlpha", "160");
        wxl::runtime::lua::RegisterFunction("SetQuestMarker", &QuestMarker::ScriptSetMarker);
        wxl::runtime::lua::RegisterFunction("ClearAllQuestMarkers", &QuestMarker::ScriptClearAll);
        wxl::runtime::lua::RegisterFunction("GetDistInfo", &QuestMarker::ScriptGetDistInfo);
        wxl::runtime::lua::RegisterFunction("SetMarkerAlpha", &QuestMarker::ScriptSetAlpha);
        wxl::runtime::lua::RegisterScript("wxl-quest-marker", R"lua(
            local cache = {}
            local currentQuest = 0
            local lastQuest = 0
            local distFrame = nil
            local distText = nil
            local settingsAlphaSlider = nil
            local refreshNavigationIndicators = function() end

            local function readCVar(name, fallback)
                if not GetCVar then return fallback end
                local ok, value = pcall(GetCVar, name)
                if ok and value ~= nil then return value end
                return fallback
            end

            local markerEnabled = readCVar("wxlQuestMarkerEnabled", "1") ~= "0"
            local markerAlpha = tonumber(readCVar("wxlQuestMarkerAlpha", "160")) or 160
            markerAlpha = math.max(0, math.min(255, markerAlpha))
            SetMarkerAlpha(markerAlpha)

            local function saveMarkerSettings()
                if SetCVar then
                    pcall(SetCVar, "wxlQuestMarkerEnabled", markerEnabled and "1" or "0")
                    pcall(SetCVar, "wxlQuestMarkerAlpha", tostring(math.floor(markerAlpha + 0.5)))
                end
            end

            local function formatDist(d)
                if d >= 1000 then return string.format("%.1f km", d / 1000)
                else return string.format("%.0f yd", d) end
            end

            local function SwitchTo(questId)
                local c = cache[questId]
                if c then
                    if currentQuest ~= 0 and currentQuest ~= questId then
                        lastQuest = currentQuest
                    end
                    currentQuest = questId
                    ClearAllQuestMarkers()
                    if markerEnabled then
                        SetQuestMarker(questId, 1, c[1], c[2], c[3], c[4] or 0)
                    end
                    refreshNavigationIndicators()
                end
            end

            local function applyMarkerEnabled(enabled)
                markerEnabled = not not enabled
                saveMarkerSettings()
                ClearAllQuestMarkers()
                if markerEnabled then
                    if currentQuest ~= 0 and cache[currentQuest] then
                        SwitchTo(currentQuest)
                    else
                        for id in pairs(cache) do
                            SwitchTo(id)
                            break
                        end
                    end
                end
                refreshNavigationIndicators()
            end

            local function applyMarkerAlpha(alpha)
                markerAlpha = math.max(0, math.min(255, tonumber(alpha) or 160))
                SetMarkerAlpha(markerAlpha)
                saveMarkerSettings()
            end

            OnWXLPacket(0x0102, function(reader)
                local qid = reader:ReadUInt32()
                local active = reader:ReadUInt32()
                local x = reader:ReadDouble()
                local y = reader:ReadDouble()
                local z = reader:ReadDouble()
                local markerType = reader:ReadUInt32()
                if active ~= 0 then
                    cache[qid] = {x, y, z, markerType}
                    if currentQuest == 0 or currentQuest == qid then
                        SwitchTo(qid)
                    end
                else
                    cache[qid] = nil
                    if currentQuest == qid then
                        currentQuest = 0
                        ClearAllQuestMarkers()
                        if lastQuest ~= 0 and cache[lastQuest] then
                            SwitchTo(lastQuest)
                        else
                            lastQuest = 0
                            for id, c in pairs(cache) do
                                SwitchTo(id)
                                break
                            end
                        end
                    end
                end
                refreshNavigationIndicators()
            end)

            local frame = CreateFrame('Frame')
            frame:RegisterEvent('PLAYER_ENTERING_WORLD')
            frame:SetScript('OnEvent', function(self, event)
                frame:UnregisterAllEvents()
                frame:SetScript('OnEvent', nil)
                SLASH_QUESTMARKER1 = '/questmarker'
                SLASH_QUESTMARKER2 = '/qm'
                SlashCmdList['QUESTMARKER'] = function(msg)
                    local parts = {}
                    for s in msg:gmatch('%S+') do parts[#parts+1] = s end
                    local x = tonumber(parts[1]) or -8900
                    local y = tonumber(parts[2]) or -130
                    local z = tonumber(parts[3]) or 82
                    SetQuestMarker(1, 1, x, y, z, 0)
                    DEFAULT_CHAT_FRAME:AddMessage('Marker at '..x..' '..y..' '..z)
                end
                SLASH_QUESTMARKERCLEAR1 = '/questmarkerclear'
                SLASH_QUESTMARKERCLEAR2 = '/qmc'
                SlashCmdList['QUESTMARKERCLEAR'] = function()
                    ClearAllQuestMarkers()
                    DEFAULT_CHAT_FRAME:AddMessage('Marker cleared')
                end
                SLASH_QUESTMARKERALPHA1 = '/questmarkeralpha'
                SLASH_QUESTMARKERALPHA2 = '/qma'
                SlashCmdList['QUESTMARKERALPHA'] = function(msg)
                    local a = tonumber(msg) or 160
                    if a < 0 then a = 0 end
                    if a > 255 then a = 255 end
                    applyMarkerAlpha(a)
                    if settingsAlphaSlider then settingsAlphaSlider:SetValue(markerAlpha) end
                    DEFAULT_CHAT_FRAME:AddMessage('Marker alpha set to ' .. a)
                end

                local function registerQuestMarkerOptions()
                    if _G.WarcraftXLQuestMarkerOptions then return true end
                    if not WarcraftXL_AddOptionsCategory then return false end

                    local options = CreateFrame("Frame", "WarcraftXLQuestMarkerOptions")
                    options.name = "Quest Marker"

                    local optionsTitle = options:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
                    optionsTitle:SetPoint("TOPLEFT", 16, -16)
                    optionsTitle:SetText("Quest Marker")

                    local optionsDescription = options:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
                    optionsDescription:SetPoint("TOPLEFT", optionsTitle, "BOTTOMLEFT", 0, -8)
                    optionsDescription:SetWidth(520)
                    optionsDescription:SetJustifyH("LEFT")
                    optionsDescription:SetText("Display a world-space marker for the selected quest objective or turn-in location.")

                    local enabledCheck = CreateFrame("CheckButton", "WarcraftXLQuestMarkerEnabled", options, "UICheckButtonTemplate")
                    enabledCheck:SetPoint("TOPLEFT", optionsDescription, "BOTTOMLEFT", -2, -16)
                    local enabledLabel = options:CreateFontString(nil, "ARTWORK", "GameFontHighlight")
                    enabledLabel:SetPoint("LEFT", enabledCheck, "RIGHT", 2, 1)
                    enabledLabel:SetText("Enable quest markers")
                    enabledCheck:SetScript("OnClick", function(self)
                        applyMarkerEnabled(self:GetChecked())
                    end)

                    local alphaLabel = options:CreateFontString(nil, "ARTWORK", "GameFontNormal")
                    alphaLabel:SetPoint("TOPLEFT", enabledCheck, "BOTTOMLEFT", 2, -24)
                    alphaLabel:SetText("Marker opacity")

                    local alphaSlider = CreateFrame("Slider", "WarcraftXLQuestMarkerAlpha", options, "OptionsSliderTemplate")
                    alphaSlider:SetPoint("TOPLEFT", alphaLabel, "BOTTOMLEFT", 4, -18)
                    alphaSlider:SetWidth(260)
                    alphaSlider:SetMinMaxValues(0, 255)
                    alphaSlider:SetValueStep(5)
                    _G[alphaSlider:GetName() .. "Low"]:SetText("0")
                    _G[alphaSlider:GetName() .. "High"]:SetText("255")
                    alphaSlider:SetScript("OnValueChanged", function(self, value)
                        local rounded = math.floor((value + 2.5) / 5) * 5
                        _G[self:GetName() .. "Text"]:SetText("Opacity: " .. rounded)
                        if math.abs(markerAlpha - rounded) >= 1 then
                            applyMarkerAlpha(rounded)
                        end
                    end)
                    settingsAlphaSlider = alphaSlider

                    local function refreshOptions()
                        enabledCheck:SetChecked(markerEnabled)
                        alphaSlider:SetValue(markerAlpha)
                        _G[alphaSlider:GetName() .. "Text"]:SetText(
                            "Opacity: " .. math.floor(markerAlpha + 0.5))
                    end

                    options:SetScript("OnShow", refreshOptions)
                    options.default = function()
                        applyMarkerEnabled(true)
                        applyMarkerAlpha(160)
                        refreshOptions()
                    end

                    WarcraftXL_AddOptionsCategory(options)
                    _G.WarcraftXLQuestMarkerOptions = options
                    return true
                end

                if not registerQuestMarkerOptions() then
                    local optionsLoader = CreateFrame("Frame")
                    local optionsElapsed = 0
                    optionsLoader:SetScript("OnUpdate", function(self, delta)
                        optionsElapsed = optionsElapsed + delta
                        if optionsElapsed < 0.1 then return end
                        optionsElapsed = 0
                        if registerQuestMarkerOptions() then
                            self:SetScript("OnUpdate", nil)
                        end
                    end)
                end
        )lua"
        R"lua(
                -- Debounced snapshot requests keep the AzerothCore marker state in sync
                -- without flooding on QUEST_LOG_UPDATE bursts.
                local syncPending = false
                local syncDelay = 0
                local function queueMarkerSync(delay)
                    syncPending = true
                    syncDelay = delay or 0.15
                end
                local syncFrame = CreateFrame('Frame')
                syncFrame:RegisterEvent('PLAYER_ENTERING_WORLD')
                syncFrame:RegisterEvent('PLAYER_LEAVING_WORLD')
                syncFrame:RegisterEvent('QUEST_LOG_UPDATE')
                syncFrame:RegisterEvent('ZONE_CHANGED_NEW_AREA')
                syncFrame:SetScript('OnEvent', function(_, syncEvent)
                    if syncEvent == 'PLAYER_LEAVING_WORLD' then
                        ClearAllQuestMarkers()
                        cache = {}
                        currentQuest = 0
                        lastQuest = 0
                        syncPending = false
                    else
                        queueMarkerSync(syncEvent == 'PLAYER_ENTERING_WORLD' and 0.35 or 0.15)
                    end
                end)
                syncFrame:SetScript('OnUpdate', function(_, elapsed)
                    if not syncPending then return end
                    syncDelay = syncDelay - elapsed
                    if syncDelay > 0 then return end
                    syncPending = false
                    local rpkt = CreateWXLPacket(0x051F, 0)
                    if rpkt then rpkt:Send() end
                end)
                queueMarkerSync(0)

                local lastSel = -1
                local qf = CreateFrame('Frame')
                qf:SetSize(1, 1)
                qf:Show()
                -- Poll selection at a low cadence; the 3.3.5 API has no reliable
                -- event for every tracker/quest-log selection path.
                local selectionElapsed = 0
                qf:SetScript('OnUpdate', function(_, elapsed)
                    selectionElapsed = selectionElapsed + elapsed
                    if selectionElapsed < 0.10 then return end
                    selectionElapsed = 0
                    -- Login refresh: show first cached quest if none shown
                    if currentQuest == 0 then
                        for id, c in pairs(cache) do
                            SwitchTo(id)
                            break
                        end
                    end
                    -- Quest selection detection
                    local idx = GetQuestLogSelection()
                    if idx and idx > 0 and idx ~= lastSel then
                        lastSel = idx
                        local title, _, _, _, _, _, _, _, questID = GetQuestLogTitle(idx)
                        if questID and questID > 0 and questID ~= currentQuest and cache[questID] then
                            SwitchTo(questID)
                        end
                    end
                end)
                -- Hook QuestLog_SetSelection (fires on tracker/quest log clicks)
                local qlssHooked = false
                local function hookQLSS()
                    if qlssHooked or type(QuestLog_SetSelection) ~= 'function' then
                        return qlssHooked
                    end
                    hooksecurefunc("QuestLog_SetSelection", function(questLogIndex)
                        if questLogIndex and questLogIndex > 0 then
                            local title, _, _, _, _, _, _, _, questID = GetQuestLogTitle(questLogIndex)
                            if questID and questID > 0 and questID ~= currentQuest and cache[questID] then
                                SwitchTo(questID)
                            end
                        end
                    end)
                    qlssHooked = true
                    return true
                end
                if not hookQLSS() then
                    -- FrameXML can expose the function shortly after the module installs.
                    local qlssRetry = CreateFrame('Frame')
                    local retryElapsed = 0
                    qlssRetry:SetScript('OnUpdate', function(_, elapsed)
                        retryElapsed = retryElapsed + elapsed
                        if retryElapsed < 0.25 then return end
                        retryElapsed = 0
                        if hookQLSS() then
                            qlssRetry:SetScript('OnUpdate', nil)
                        end
                    end)
                end
                -- Mark the quest currently selected as the navigation destination in both the
                -- quest log and the on-screen objective tracker.
                local diamondTexture = "textures\\questmarker\\diamond.blp"
                local function updateNavigationIndicators()
                    local navigating = markerEnabled and currentQuest and currentQuest ~= 0

                    if QuestLogScrollFrame and QuestLogScrollFrame.buttons then
                        for _, button in ipairs(QuestLogScrollFrame.buttons) do
                            if not button._wxlNavigationDiamond then
                                local texture = button:CreateTexture(nil, "OVERLAY")
                                texture:SetTexture(diamondTexture)
                                texture:SetWidth(12)
                                texture:SetHeight(12)
                                texture:SetPoint("LEFT", button.check, "RIGHT", 1, 0)
                                button._wxlNavigationDiamond = texture
                            end

                            local questID
                            local index = button:GetID()
                            if button:IsShown() and index and index > 0 and not button.isHeader then
                                questID = select(9, GetQuestLogTitle(index))
                            end
                            if navigating and questID == currentQuest then
                                button._wxlNavigationDiamond:Show()
                            else
                                button._wxlNavigationDiamond:Hide()
                            end
                        end
                    end

                    if WATCHFRAME_QUESTLINES then
                        for _, line in pairs(WATCHFRAME_QUESTLINES) do
                            if line._wxlNavigationDiamond then
                                line._wxlNavigationDiamond:Hide()
                            end
                        end
                    end
                    if navigating and WATCHFRAME_LINKBUTTONS and WATCHFRAME_QUESTLINES then
                        for _, button in pairs(WATCHFRAME_LINKBUTTONS) do
                            if button.type == "QUEST" and button.index and button.startLine then
                                local index = GetQuestIndexForWatch(button.index)
                                local questID = index and select(9, GetQuestLogTitle(index))
                                local line = WATCHFRAME_QUESTLINES[button.startLine]
                                if questID == currentQuest and line then
                                    if not line._wxlNavigationDiamond then
                                        local texture = line:CreateTexture(nil, "OVERLAY")
                                        texture:SetTexture(diamondTexture)
                                        texture:SetWidth(12)
                                        texture:SetHeight(12)
                                        -- Blizzard's circular quest POI occupies the normal icon
                                        -- slot immediately left of the title. Put navigation one
                                        -- icon-width farther left so both remain visible.
                                        texture:SetPoint("RIGHT", line, "LEFT", -25, 0)
                                        line._wxlNavigationDiamond = texture
                                    end
                                    line._wxlNavigationDiamond:Show()
                                end
                            end
                        end
                    end
                end
                refreshNavigationIndicators = updateNavigationIndicators

                local navigationHooksInstalled = false
                local function installNavigationHooks()
                    if navigationHooksInstalled then return true end
                    if type(QuestLog_Update) ~= "function" or
                       type(WatchFrame_Update) ~= "function" then
                        return false
                    end
                    hooksecurefunc("QuestLog_Update", updateNavigationIndicators)
                    hooksecurefunc("WatchFrame_Update", updateNavigationIndicators)
                    navigationHooksInstalled = true
                    updateNavigationIndicators()
                    return true
                end
                if not installNavigationHooks() then
                    local navigationRetry = CreateFrame("Frame")
                    local navigationElapsed = 0
                    navigationRetry:SetScript("OnUpdate", function(self, elapsed)
                        navigationElapsed = navigationElapsed + elapsed
                        if navigationElapsed < 0.25 then return end
                        navigationElapsed = 0
                        if installNavigationHooks() then self:SetScript("OnUpdate", nil) end
                    end)
                end
                -- Hook AddQuestWatch (world map "Track Quest")
                hooksecurefunc("AddQuestWatch", function(questIndex, watchTime)
                    if questIndex and questIndex > 0 then
                        local title, _, _, _, _, _, _, _, questID = GetQuestLogTitle(questIndex)
                        if questID and questID > 0 and questID ~= currentQuest and cache[questID] then
                            SwitchTo(questID)
                        end
                    end
                end)
                -- Distance text frame
                if not distFrame then
                    distFrame = CreateFrame("Frame", nil, UIParent)
                    distFrame:SetSize(200, 30)
                    distFrame:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
                    distFrame:SetFrameStrata("TOOLTIP")
                    distText = distFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
                    distText:SetPoint("TOP", distFrame, "TOP", 0, 0)
                    distText:SetTextColor(1, 0.84, 0, 160/255)
                    distText:SetShadowOffset(1, -1)
                    distFrame:SetScript("OnUpdate", function()
                        if not markerEnabled or not currentQuest or currentQuest == 0 then
                            distText:SetText("")
                            return
                        end
                        local c = cache[currentQuest]
                        if not c then
                            distText:SetText("")
                            return
                        end
                        local ndcX, ndcY, dist = GetDistInfo()
                        if ndcX then
                            distText:SetText(formatDist(dist))
                            local sw, sh = GetScreenWidth(), GetScreenHeight()
                            local uiX = math.max(0, math.min(sw, (ndcX + 1) * 0.5 * sw))
                            local uiY = math.max(0, math.min(sh, (ndcY + 1) * 0.5 * sh))
                            distFrame:ClearAllPoints()
                            distFrame:SetPoint("TOP", UIParent, "BOTTOMLEFT", uiX, uiY - 20)
                        else
                            distText:SetText("")
                        end
                    end)
                end
            end)
        )lua");
        WLOG_INFO("quest-marker: installed");
    }

    struct Registrar
    {
        Registrar() {
            client_extensions::network::RegisterClientOpcode(
                0x051F, "CMSG_WXL_QUEST_MARKER_REQUEST");
            client_extensions::network::RegisterServerOpcode(
                0x0102, "SMSG_WXL_QUEST_MARKER_UPDATE");
            wxl::runtime::modules::Register("wxl-quest-marker", &InstallModule);
        }
    } g_registrar;

    QuestMarker::QuestMarker()
    {
        on<&QuestMarker::OnWorldRenderEnd>(wxl::events::Event::OnWorldRenderEnd);
        on<&QuestMarker::OnDeviceReset>(wxl::events::Event::OnDeviceReset);
        on<&QuestMarker::OnDeviceLost>(wxl::events::Event::OnDeviceLost);
    }

    QuestMarker::~QuestMarker() { ReleaseDevice(); }

    void QuestMarker::SetMarker(uint32_t questId, bool active,
                                float x, float y, float z, uint32_t markerType)
    {
        if (active) {
            for (auto& m : m_markers)
                if (m.questId == questId) {
                    m.active = true; m.x = x; m.y = y; m.z = z;
                    m.markerType = markerType; return;
                }
            m_markers.push_back({questId, true, x, y, z, markerType});
        } else {
            for (auto it = m_markers.begin(); it != m_markers.end(); ++it)
                if (it->questId == questId) { m_markers.erase(it); break; }
        }
    }

    void QuestMarker::ClearMarker(uint32_t questId)
    {
        for (auto it = m_markers.begin(); it != m_markers.end(); ++it)
            if (it->questId == questId) { m_markers.erase(it); break; }
    }

    void QuestMarker::ClearAllMarkers()
    {
        m_markers.clear();
    }

    void QuestMarker::OnDeviceLost(const wxl::events::DeviceResetArgs&) { ReleaseDevice(); }
    void QuestMarker::OnDeviceReset(const wxl::events::DeviceResetArgs& a) {
        if (a.device) InitDevice(a.device);
    }

    void QuestMarker::InitDevice(void* device)
    {
        if (m_deviceReady) return;

        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!d) { m_deviceReady = true; return; }

        m_d3dTexture = LoadBlpTexture(d, "textures\\questmarker\\diamond.blp");
        if (!m_d3dTexture)
        {
            WLOG_INFO("quest-marker: BLP not found, using debug texture");
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
        m_markerValid = false;
        if (m_markers.empty()) return;

        // Skip on glue screens (login/logout/character select)
        if (a.proj != nullptr)
        {
            m_markers.clear();
            return;
        }

        gx::Device9 dev(a.device);
        if (!dev) return;

        if (!m_deviceReady) InitDevice(a.device);
        if (!m_deviceReady) return;

        ScopedDeviceState state(dev);

        auto* rawDev = static_cast<IDirect3DDevice9*>(dev.raw());

        rawDev->SetVertexShader(nullptr);
        rawDev->SetPixelShader(nullptr);
        rawDev->SetTexture(0, static_cast<IDirect3DBaseTexture9*>(m_d3dTexture));
        rawDev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        rawDev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        rawDev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        rawDev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        rawDev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        rawDev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        rawDev->SetRenderState(D3DRS_LIGHTING, FALSE);
        rawDev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        rawDev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        rawDev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        rawDev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        rawDev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        rawDev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

        const float* viewMat = cam::View();
        const float* projMat = cam::Projection();
        if (!viewMat || !projMat) return;
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
        cam::Position(camPos);

        // Also skip if camera at origin (glue screen fallback)
        if (camPos[0] == 0.0f && camPos[1] == 0.0f && camPos[2] == 0.0f) return;

        // Get player position for distance text
        float plrPos[3] = { camPos[0], camPos[1], camPos[2] };
        auto plrGuid = world::ActivePlayerGuid();
        if (plrGuid)
        {
            void* playerObj = world::ResolveObject(plrGuid, world::kTypeMaskPlayer);
            if (playerObj) world::UnitPosition(playerObj, plrPos);
        }

        int visibleCount = 0;
        for (const auto& mk : m_markers) {
            if (!mk.active) continue;
            float dx = mk.x - plrPos[0], dy = mk.y - plrPos[1], dz = mk.z - plrPos[2];
            if (dx*dx + dy*dy + dz*dz <= kMaxDistSq) ++visibleCount;
        }
        if (visibleCount == 0) return;

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
                D3DPOOL_DEFAULT, &vb, nullptr)) || !vb) return;
            m_vb = vb;
            m_vbSize = newSize;
        }

        MarkerVert* verts = nullptr;
        if (FAILED(static_cast<IDirect3DVertexBuffer9*>(m_vb)->Lock(
            0, 0, (void**)&verts, D3DLOCK_DISCARD)) || !verts) return;

        uint32_t markerColor = (m_alpha << 24) | 0xFFCC00;
        int vi = 0;
        for (const auto& mk : m_markers) {
            if (!mk.active) continue;
            float pdx = mk.x - plrPos[0], pdy = mk.y - plrPos[1], pdz = mk.z - plrPos[2];
            if (pdx*pdx + pdy*pdy + pdz*pdz > kMaxDistSq) continue;
            verts[vi]   = MarkerVert{ 0,  1, 0, markerColor, 0.5f, 0.0f };
            verts[vi+1] = MarkerVert{ -1, 0, 0, markerColor, 0.0f, 0.5f };
            verts[vi+2] = MarkerVert{ 0, -1, 0, markerColor, 0.5f, 1.0f };
            verts[vi+3] = MarkerVert{ 0,  1, 0, markerColor, 0.5f, 0.0f };
            verts[vi+4] = MarkerVert{ 1,  0, 0, markerColor, 1.0f, 0.5f };
            verts[vi+5] = MarkerVert{ 0, -1, 0, markerColor, 0.5f, 1.0f };
            vi += kVertsPerMk;
        }

        static_cast<IDirect3DVertexBuffer9*>(m_vb)->Unlock();
        rawDev->SetStreamSource(0, static_cast<IDirect3DVertexBuffer9*>(m_vb), 0, sizeof(MarkerVert));
        rawDev->SetFVF(D3DFVF_MARKERVERTS);

        int baseVertex = 0;
        bool ndcStored = false;

        D3DVIEWPORT9 vp;
        rawDev->GetViewport(&vp);
        float vpHeight = static_cast<float>(vp.Height);
        float yScale = projMat[5]; // vertical FOV scale from projection matrix

        for (const auto& mk : m_markers) {
            if (!mk.active) { baseVertex += kVertsPerMk; continue; }
            float pdx = mk.x - plrPos[0], pdy = mk.y - plrPos[1], pdz = mk.z - plrPos[2];
            if (pdx*pdx + pdy*pdy + pdz*pdz > kMaxDistSq) { baseVertex += kVertsPerMk; continue; }

            // Cylindrical billboard: face camera horizontally, stay upright along world Z
            float dx = mk.x - camPos[0], dy = mk.y - camPos[1];
            float fx = -dx, fy = -dy;
            float flen = sqrtf(fx*fx + fy*fy);
            if (flen < 0.001f) { fx = 0.0f; fy = 1.0f; flen = 1.0f; }
            fx /= flen; fy /= flen;
            // Right = cross(up, forward) where up = (0,0,1)
            float rx = -fy, ry = fx;
            // Dynamic scale: size so marker appears ~kMarkerPixelSize pixels tall
            float viewZ = mk.x * viewMat[2] + mk.y * viewMat[6] + mk.z * viewMat[10] + viewMat[14];

            // Store projection for distance text (first visible marker only)
            // Done before viewZ check so behind-camera markers still update distance/NDC
            if (!ndcStored) {
                float vx = mk.x*viewMat[0] + mk.y*viewMat[4] + mk.z*viewMat[8]  + viewMat[12];
                float vy = mk.x*viewMat[1] + mk.y*viewMat[5] + mk.z*viewMat[9]  + viewMat[13];
                float vz = mk.x*viewMat[2] + mk.y*viewMat[6] + mk.z*viewMat[10] + viewMat[14];
                float vw = mk.x*viewMat[3] + mk.y*viewMat[7] + mk.z*viewMat[11] + viewMat[15];
                float px = vx*projMat[0] + vy*projMat[4] + vz*projMat[8]  + vw*projMat[12];
                float py = vx*projMat[1] + vy*projMat[5] + vz*projMat[9]  + vw*projMat[13];
                float pw = vx*projMat[3] + vy*projMat[7] + vz*projMat[11] + vw*projMat[15];
                float ndcX = 0.0f, ndcY = 0.0f;
                if (pw >= 0.0001f) {
                    ndcX = px / pw;
                    ndcY = py / pw;
                } else {
                    // Marker behind camera: place text at screen edge in correct direction
                    ndcX = (vx >= 0.0f) ? 2.0f : -2.0f;
                    ndcY = (vy >= 0.0f) ? 2.0f : -2.0f;
                }
                m_ndcX = ndcX;
                m_ndcY = ndcY;
                m_dist = sqrtf(pdx*pdx + pdy*pdy + pdz*pdz);
                ndcStored = true;
            }

            if (viewZ < 0.1f) { baseVertex += kVertsPerMk; continue; }
            float s = kMarkerPixelSize * 2.0f * viewZ / (yScale * vpHeight);
            if (s < 0.01f) s = 0.01f;
            if (s > 200.0f) s = 200.0f;
            D3DMATRIX w = {};
            w._11 = rx * s;  w._12 = ry * s;  w._13 = 0.0f;
            w._21 = 0.0f;    w._22 = 0.0f;    w._23 = s;
            w._31 = fx * s;  w._32 = fy * s;  w._33 = 0.0f;
            w._41 = mk.x;    w._42 = mk.y;    w._43 = mk.z;    w._44 = 1.0f;
            rawDev->SetTransform(D3DTS_WORLD, &w);
            rawDev->DrawPrimitive(D3DPT_TRIANGLELIST, baseVertex, 2);
            baseVertex += kVertsPerMk;
        }

        m_markerValid = ndcStored;
        state.Restore();
    }
}
