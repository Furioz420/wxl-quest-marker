-- WarcraftXL port of bozo-1/WXL-UI-Tracker v2 for the Eunoia 3.3.5a UI.
-- The server owns zone eligibility and objective coordinates. The addon owns
-- only the selected quest and never substitutes a different cached marker.

local Tracker = {
    cache = {},
    currentQuest = 0,
    selectedQuest = 0,
    selectedSource = "none",
    killEntries = {},
    batch = false,
    batchComplete = false,
    renderState = "idle",
}
_G.WXL_UI_Tracker = Tracker

local function normalizeSettings()
    -- Load-on-demand SavedVariables can replace the table after the file's
    -- initial defaults have run on some 3.3.5a UI stacks. Normalize the live
    -- table whenever settings are consumed so a missing legacy field cannot
    -- silently disable the native world marker.
    if type(QuestMarkerDB) ~= "table" then QuestMarkerDB = {} end
    if QuestMarkerDB.enabled == nil then QuestMarkerDB.enabled = true end
    if QuestMarkerDB.showMarker == nil then QuestMarkerDB.showMarker = true end
    if QuestMarkerDB.showOffscreen == nil then QuestMarkerDB.showOffscreen = true end
    if QuestMarkerDB.showTrackerDiamond == nil then QuestMarkerDB.showTrackerDiamond = true end
    if QuestMarkerDB.showDistance == nil then QuestMarkerDB.showDistance = true end
    if QuestMarkerDB.distanceUnit == nil then QuestMarkerDB.distanceUnit = "yards" end
    if QuestMarkerDB.alpha == nil then QuestMarkerDB.alpha = 165 end
    if QuestMarkerDB.worldMarkerScale == nil then QuestMarkerDB.worldMarkerScale = 1 end
    if QuestMarkerDB.offscreenScale == nil then QuestMarkerDB.offscreenScale = 1 end
    if QuestMarkerDB.trackerDiamondScale == nil then QuestMarkerDB.trackerDiamondScale = 1 end
    if QuestMarkerDB.offscreenOffsetX == nil then QuestMarkerDB.offscreenOffsetX = 0 end
    if QuestMarkerDB.offscreenOffsetY == nil then QuestMarkerDB.offscreenOffsetY = 0 end

    QuestMarkerDB.alpha = math.max(25, math.min(255, tonumber(QuestMarkerDB.alpha) or 165))
    QuestMarkerDB.worldMarkerScale = math.max(0.5, math.min(2, tonumber(QuestMarkerDB.worldMarkerScale) or 1))
    QuestMarkerDB.offscreenScale = math.max(0.5, math.min(2, tonumber(QuestMarkerDB.offscreenScale) or 1))
    QuestMarkerDB.trackerDiamondScale = math.max(0.5, math.min(2, tonumber(QuestMarkerDB.trackerDiamondScale) or 1))
    QuestMarkerDB.offscreenOffsetX = math.max(-500, math.min(500, tonumber(QuestMarkerDB.offscreenOffsetX) or 0))
    QuestMarkerDB.offscreenOffsetY = math.max(-350, math.min(350, tonumber(QuestMarkerDB.offscreenOffsetY) or 0))
    if QuestMarkerDB.distanceUnit ~= "yards" and
       QuestMarkerDB.distanceUnit ~= "meters" and
       QuestMarkerDB.distanceUnit ~= "metric" then
        QuestMarkerDB.distanceUnit = "yards"
    end
end
normalizeSettings()

local requestCooldown = {}
local requestSnapshotAt = 0
local selectionPoll = 0
local diamondPoll = 0
local nativePoll = 0
local poiBindPoll = 0
local requestSelectedAt = 0
local observed = { customIndex = -1, customQuest = -1, mapQuest = -1,
    logIndex = -1, logQuest = -1 }

local function native()
    return wxlwow and wxlwow.quest_tracker
end

local function questIDForIndex(index)
    index = tonumber(index) or 0
    if index <= 0 or not GetQuestLogTitle then return 0 end
    local _, _, _, _, isHeader, _, _, _, questID = GetQuestLogTitle(index)
    if isHeader then return 0 end
    return tonumber(questID) or 0
end

local function questIndexForID(questID)
    questID = tonumber(questID) or 0
    if questID <= 0 then return 0 end
    for index = 1, GetNumQuestLogEntries() do
        if questIDForIndex(index) == questID then return index end
    end
    return 0
end

local function sendRequest(kind, questID, force)
    local bridge = native()
    if not bridge or type(bridge.Request) ~= "function" then return false end
    local now = GetTime()
    local key = tostring(kind) .. ":" .. tostring(questID or 0)
    if not force and requestCooldown[key] and now - requestCooldown[key] < 1.5 then
        return false
    end
    requestCooldown[key] = now
    return bridge.Request(kind, questID or 0)
end

local function applySettings()
    normalizeSettings()
    if QuestTrackerReloadSettings then
        QuestTrackerReloadSettings(
            QuestMarkerDB.enabled and 1 or 0,
            QuestMarkerDB.showMarker and 1 or 0,
            QuestMarkerDB.alpha,
            QuestMarkerDB.worldMarkerScale,
            QuestMarkerDB.offscreenScale,
            QuestMarkerDB.offscreenOffsetX,
            QuestMarkerDB.offscreenOffsetY,
            QuestMarkerDB.showOffscreen and 1 or 0)
    end
end

local function clearRenderedMarker()
    if ClearAllQuestMarkers then ClearAllQuestMarkers() end
    Tracker.currentQuest = 0
end

local function applySelectedMarker()
    applySettings()
    if not QuestMarkerDB.enabled or not QuestMarkerDB.showMarker then
        Tracker.renderState = "disabled"
        clearRenderedMarker()
        return
    end

    local questID = tonumber(Tracker.selectedQuest) or 0
    local marker = questID > 0 and Tracker.cache[questID] or nil
    if not marker then
        Tracker.renderState = "cache-miss"
        clearRenderedMarker()
        if questID > 0 then sendRequest(101, questID, false) end
        return
    end

    if Tracker.currentQuest ~= 0 and Tracker.currentQuest ~= questID and SetQuestMarker then
        SetQuestMarker(Tracker.currentQuest, 0, 0, 0, 0)
    end
    if type(SetMarkerAlpha) == "function" then
        SetMarkerAlpha(tonumber(QuestMarkerDB.alpha) or 165)
    end
    if type(SetQuestMarker) == "function" then
        local ok = pcall(SetQuestMarker, questID, 1, marker.x, marker.y, marker.z)
        if not ok then
            Tracker.renderState = "set-failed"
            Tracker.currentQuest = 0
            return
        end
        Tracker.currentQuest = questID
        Tracker.renderState = "active"
    else
        Tracker.currentQuest = 0
        Tracker.renderState = "native-api-missing"
    end
end

local function requestKillEntries(questID)
    if questID and questID > 0 then
        sendRequest(103, questID, false)
    else
        sendRequest(103, 0, true)
    end
end

local function mirrorSelection(questID)
    if not questID or questID <= 0 then return end
    WORLDMAP_SETTINGS = WORLDMAP_SETTINGS or {}
    WORLDMAP_SETTINGS.selectedQuestId = questID
    local index = questIndexForID(questID)
    if index > 0 then
        QuestMap_SelectedQuest = index
        if GetQuestLogSelection and GetQuestLogSelection() ~= index and SelectQuestLogEntry then
            SelectQuestLogEntry(index)
        end
    end
end

local function selectQuest(questID, source, mirror)
    questID = tonumber(questID) or 0
    if questID < 0 then questID = 0 end
    local changed = Tracker.selectedQuest ~= questID
    Tracker.selectedQuest = questID
    Tracker.selectedSource = source or Tracker.selectedSource
    if mirror and questID > 0 then mirrorSelection(questID) end
    if changed then requestKillEntries(questID) end
    applySelectedMarker()
end

function Tracker.SelectQuest(questID, questLogIndex, source)
    questID = tonumber(questID) or 0
    if questID <= 0 then questID = questIDForIndex(questLogIndex) end
    selectQuest(questID, source or "external", true)
end
_G.WXL_UITracker_SelectQuest = Tracker.SelectQuest

local function firstMapQuestID()
    if not QuestMapUpdateAllQuests or not QuestPOIGetQuestIDByVisibleIndex then return 0 end
    pcall(QuestMapUpdateAllQuests)
    local ok, questID, questIndex = pcall(QuestPOIGetQuestIDByVisibleIndex, 1)
    if ok and tonumber(questIndex) and questIndex > 0 then
        return tonumber(questID) or 0
    end
    return 0
end

local function readSelection()
    local customIndex = tonumber(_G.QuestMap_SelectedQuest) or 0
    local customQuest = questIDForIndex(customIndex)
    local mapQuest = type(_G.WORLDMAP_SETTINGS) == "table" and
        (tonumber(_G.WORLDMAP_SETTINGS.selectedQuestId) or 0) or 0
    local logIndex = GetQuestLogSelection and (tonumber(GetQuestLogSelection()) or 0) or 0
    local logQuest = questIDForIndex(logIndex)
    return customIndex, customQuest, mapQuest, logIndex, logQuest
end

local function pollSelection()
    local customIndex, customQuest, mapQuest, logIndex, logQuest = readSelection()
    local questID, source
    local customChanged = customIndex ~= observed.customIndex or customQuest ~= observed.customQuest
    local mapChanged = mapQuest ~= observed.mapQuest
    local logChanged = logIndex ~= observed.logIndex or logQuest ~= observed.logQuest

    local customOpen = QuestMapFrame and QuestMapFrame.IsShown and QuestMapFrame:IsShown()
    local worldMapOpen = WorldMapFrame and WorldMapFrame.IsShown and WorldMapFrame:IsShown()
    -- Eunoia stores two different values: QuestMap_SelectedQuest is the quest
    -- log row, while WORLDMAP_SETTINGS.selectedQuestId is the actual quest ID.
    -- When the custom map is open its explicit quest ID is authoritative.
    if customOpen and mapQuest > 0 and (customChanged or mapChanged) then
        questID, source = mapQuest, "eunoia-map"
    elseif customChanged and customQuest > 0 then
        questID, source = customQuest, "eunoia-map"
    elseif mapChanged and mapQuest > 0 then
        questID, source = mapQuest, customOpen and "eunoia-map" or "world-map"
    elseif logChanged and logQuest > 0 then
        questID, source = logQuest, "quest-log"
    elseif Tracker.selectedQuest == 0 then
        if customOpen and mapQuest > 0 then
            questID, source = mapQuest, "eunoia-map"
        elseif worldMapOpen and mapQuest > 0 then
            questID, source = mapQuest, "world-map"
        elseif mapQuest > 0 then
            questID, source = mapQuest, "world-map"
        elseif customQuest > 0 then
            questID, source = customQuest, "eunoia-map"
        elseif logQuest > 0 then
            questID, source = logQuest, "quest-log"
        else
            questID, source = firstMapQuestID(), "map-auto"
        end
    end

    observed.customIndex, observed.customQuest = customIndex, customQuest
    observed.mapQuest, observed.logIndex, observed.logQuest = mapQuest, logIndex, logQuest
    if questID and questID > 0 then selectQuest(questID, source, true) end
end

local function pullNativePackets()
    local bridge = native()
    if not bridge then return end

    if type(bridge.PopMarker) == "function" then
        while true do
            local questID, active, x, y, z, markerType = bridge.PopMarker()
            if questID == nil then break end
            questID = tonumber(questID) or 0
            if questID == 0 then
                Tracker.batchComplete = true
                Tracker.batch = false
            elseif active == 1 then
                Tracker.cache[questID] = { x = x, y = y, z = z,
                    markerType = markerType or 0 }
            else
                Tracker.cache[questID] = nil
            end
        end
    end

    if type(bridge.PopKillEntries) == "function" then
        while true do
            local count, e1, e2, e3, e4 = bridge.PopKillEntries()
            if count == nil then break end
            Tracker.killEntries = {}
            local entries = { e1, e2, e3, e4 }
            for i = 1, tonumber(count) or 0 do
                if entries[i] and entries[i] > 0 then
                    table.insert(Tracker.killEntries, entries[i])
                end
            end
        end
    end

    if type(bridge.PopCorpse) == "function" then
        while true do
            local active, x, y, z = bridge.PopCorpse()
            if active == nil then break end
            if active == 1 and SetCorpseMarker then
                SetCorpseMarker(x, y, z)
            elseif ClearCorpseMarker then
                ClearCorpseMarker()
            end
        end
    end
    applySelectedMarker()
end

local function findTrackerTitleLine(questID)
    if not WATCHFRAME_LINKBUTTONS then return nil end
    for _, button in ipairs(WATCHFRAME_LINKBUTTONS) do
        if button.type == "QUEST" and button.lines and button.startLine then
            local questLogIndex = button.index and GetQuestIndexForWatch(button.index)
            if questLogIndex and questIDForIndex(questLogIndex) == questID then
                return button.lines[button.startLine]
            end
        end
    end
end

local function updateTrackerDiamond()
    if not SetTrackerDiamond or not QuestMarkerDB.enabled or Tracker.selectedQuest <= 0 or
       not WatchFrame or not WatchFrame:IsShown() or
       (WatchFrameLines and not WatchFrameLines:IsShown()) then
        if SetTrackerDiamond then SetTrackerDiamond(0, 0, 0, 0, 0) end
        return
    end
    local line = findTrackerTitleLine(Tracker.selectedQuest)
    if not line then SetTrackerDiamond(0, 0, 0, 0, 0); return end

    -- Selection state and the navigation diamond are separate visuals. Always
    -- select the matching tracker POI so its native gold glow is present even
    -- when the Eunoia map already synchronized selectedQuestId.
    if QuestPOI_SelectButtonByQuestId then
        QuestPOI_SelectButtonByQuestId("WatchFrameLines", Tracker.selectedQuest, true)
    end

    if not QuestMarkerDB.showTrackerDiamond then
        SetTrackerDiamond(0, 0, 0, 0, 0)
        return
    end

    local left, top, bottom = line:GetLeft(), line:GetTop(), line:GetBottom()
    local sw, sh = GetScreenWidth(), GetScreenHeight()
    if not left or not top or not bottom or not sw or not sh or sw <= 0 or sh <= 0 then
        SetTrackerDiamond(0, 0, 0, 0, 0)
        return
    end
    -- GetLeft/GetTop/GetBottom and GetScreenWidth/GetScreenHeight are already
    -- expressed in the same effective UI coordinate space in 3.3.5a. Applying
    -- UI scale again moves the diamond toward screen centre (near the player).
    local cx = (left - 42) / sw * 2 - 1
    local cy = (((top + bottom) / 2) - 1) / sh * 2 - 1
    local trackerScale = QuestMarkerDB.trackerDiamondScale or 1
    SetTrackerDiamond(1, cx, cy, (16 * trackerScale) / sw,
        (20 * trackerScale) / sh)
end

local function formatDistance(distance)
    local unit = QuestMarkerDB.distanceUnit or "yards"
    if unit == "meters" then
        return string.format("%.0f m", distance * 0.9144)
    elseif unit == "metric" then
        local meters = distance * 0.9144
        if meters >= 1000 then return string.format("%.1f km", meters / 1000) end
        return string.format("%.0f m", meters)
    end
    return string.format("%.0f yd", distance)
end

-- The native marker is projected into WorldFrame, not UIParent. Parent and
-- anchor the label to that same viewport so UI scaling, letterboxing, and
-- custom layouts cannot make it slide relative to the marker.
local distanceAnchor = WorldFrame or UIParent
local distanceFrame = CreateFrame("Frame", "QuestMarkerDistanceFrame", distanceAnchor)
distanceFrame:SetSize(200, 30)
-- The diamond is rendered in the world pass. Keep its Lua label on the
-- background strata so normal interface panels occlude both parts together.
distanceFrame:SetFrameStrata("BACKGROUND")
distanceFrame:SetFrameLevel(0)
distanceFrame.text = distanceFrame:CreateFontString("QuestMarkerDistanceText", "OVERLAY", "GameFontNormalSmall")
distanceFrame.text:SetPoint("TOP", distanceFrame, "TOP")
distanceFrame.text:SetTextColor(1, 0.84, 0)
distanceFrame.text:SetShadowOffset(1, -1)

local corpseFrame = CreateFrame("Frame", nil, distanceAnchor)
corpseFrame:SetSize(200, 30)
corpseFrame:SetFrameStrata("BACKGROUND")
corpseFrame:SetFrameLevel(0)
corpseFrame.text = corpseFrame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
corpseFrame.text:SetPoint("TOP", corpseFrame, "TOP")
corpseFrame.text:SetTextColor(1, 0.84, 0)
corpseFrame.text:SetShadowOffset(1, -1)

local function updateDistanceText()
    -- GetDistInfo returns normalized coordinates for the native WorldFrame
    -- viewport. Convert them directly to that frame's local coordinate space.
    local uiWidth = distanceAnchor:GetWidth()
    local uiHeight = distanceAnchor:GetHeight()
    if not uiWidth or not uiHeight or uiWidth <= 0 or uiHeight <= 0 then
        distanceFrame.text:SetText("")
        corpseFrame.text:SetText("")
        return
    end
    if IsCorpseActive and IsCorpseActive() then
        distanceFrame.text:SetText("")
        local x, y, distance
        if GetCorpseDistInfo then x, y, distance = GetCorpseDistInfo() end
        if x and distance then
            corpseFrame.text:SetText("Corpse - " .. formatDistance(distance))
            corpseFrame:ClearAllPoints()
            corpseFrame:SetPoint("TOP", distanceAnchor, "BOTTOMLEFT",
                math.max(0, math.min(uiWidth, (x + 1) * 0.5 * uiWidth)),
                math.max(0, math.min(uiHeight, (y + 1) * 0.5 * uiHeight)) - 38)
        else corpseFrame.text:SetText("") end
        return
    end
    corpseFrame.text:SetText("")
    if not QuestMarkerDB.showDistance then
        distanceFrame:Hide()
        return
    end
    -- The native renderer is authoritative here. Tracker.currentQuest is an
    -- asynchronous Lua mirror and may briefly be zero even while the selected
    -- native destination marker is visibly active.
    local x, y, distance
    if GetDistInfo then x, y, distance = GetDistInfo() end
    if not x or not distance or distance > 5000 then
        distanceFrame:Hide()
        return
    end
    distanceFrame.text:SetText(formatDistance(distance))
    distanceFrame.text:SetAlpha((tonumber(QuestMarkerDB.alpha) or 165) / 255)
    distanceFrame:ClearAllPoints()
    -- GetDistInfo is the marker centre. Keep the label one scaled marker
    -- half-height plus a small gap below it so the two remain a single visual
    -- unit even when the marker-size option changes.
    local markerHalfHeight = 16 * (tonumber(QuestMarkerDB.worldMarkerScale) or 1)
    distanceFrame:SetPoint("TOP", distanceAnchor, "BOTTOMLEFT",
                           math.max(0, math.min(uiWidth, (x + 1) * 0.5 * uiWidth)),
                           math.max(0, math.min(uiHeight, (y + 1) * 0.5 * uiHeight)) - markerHalfHeight - 5)
    distanceFrame:Show()
end

local driver = CreateFrame("Frame")
driver:SetScript("OnUpdate", function(_, elapsed)
    nativePoll = nativePoll - elapsed
    if nativePoll <= 0 then nativePoll = 0.10; pullNativePackets() end
    selectionPoll = selectionPoll - elapsed
    if selectionPoll <= 0 then selectionPoll = 0.10; pollSelection() end
    diamondPoll = diamondPoll - elapsed
    if diamondPoll <= 0 then diamondPoll = 0.05; updateTrackerDiamond() end
    if requestSnapshotAt > 0 and GetTime() >= requestSnapshotAt then
        requestSnapshotAt = 0
        Tracker.batch, Tracker.batchComplete = true, false
        sendRequest(102, 0, true)
    end
    if requestSelectedAt > 0 and GetTime() >= requestSelectedAt then
        requestSelectedAt = 0
        if Tracker.selectedQuest > 0 then
            sendRequest(101, Tracker.selectedQuest, true)
            requestKillEntries(Tracker.selectedQuest)
        end
    end
    updateDistanceText()
end)

local events = CreateFrame("Frame")
events:RegisterEvent("PLAYER_ENTERING_WORLD")
events:RegisterEvent("PLAYER_LEAVING_WORLD")
events:RegisterEvent("ZONE_CHANGED_NEW_AREA")
events:RegisterEvent("QUEST_LOG_UPDATE")
events:RegisterEvent("QUEST_POI_UPDATE")
events:RegisterEvent("WORLD_MAP_UPDATE")
events:RegisterEvent("PLAYER_DEAD")
events:RegisterEvent("PLAYER_ALIVE")
events:RegisterEvent("PLAYER_UNGHOST")
events:SetScript("OnEvent", function(_, event)
    if event == "PLAYER_LEAVING_WORLD" then
        Tracker.cache, Tracker.killEntries = {}, {}
        Tracker.currentQuest, Tracker.selectedQuest = 0, 0
        clearRenderedMarker()
        if ClearCorpseMarker then ClearCorpseMarker() end
        if SetDeadState then SetDeadState(0) end
        if SetTrackerDiamond then SetTrackerDiamond(0, 0, 0, 0, 0) end
        return
    end
    if event == "PLAYER_ENTERING_WORLD" or event == "ZONE_CHANGED_NEW_AREA" then
        Tracker.cache = {}
        clearRenderedMarker()
        requestCooldown = {}
        requestSnapshotAt = GetTime() + (event == "PLAYER_ENTERING_WORLD" and 0.20 or 0.50)
    end
    if event == "QUEST_LOG_UPDATE" or event == "QUEST_POI_UPDATE" then
        requestSelectedAt = GetTime() + 0.20
    end
    if event == "PLAYER_DEAD" then
        requestSnapshotAt = GetTime() + 0.75
    end
    if event == "PLAYER_DEAD" and SetDeadState then SetDeadState(1) end
    if (event == "PLAYER_ALIVE" or event == "PLAYER_UNGHOST") and SetDeadState then
        SetDeadState(0)
        if ClearCorpseMarker then ClearCorpseMarker() end
    end
    selectionPoll, diamondPoll = 0, 0
end)

local bridge = native()
if bridge then
    bridge._NativeChanged = pullNativePackets
    pullNativePackets()
end

-- Keep the tracker POI click local. It changes Blizzard's selection registers
-- without forcing the world map open, matching the upstream tracker.
function WatchFrameQuestPOI_OnClick(self)
    if not self or not self.questId then return end
    Tracker.SelectQuest(self.questId, nil, "objective-tracker")
    if QuestPOI_SelectButtonByQuestId then
        QuestPOI_SelectButtonByQuestId("WatchFrameLines", self.questId, true)
    end
    PlaySound("igMainMenuOptionCheckBoxOn")
end

local function bindTrackerPoiClicks()
    for buttonType = 1, 4 do
        for index = 1, 50 do
            local button = _G["poiWatchFrameLines" .. buttonType .. "_" .. index]
            if button and button:GetScript("OnClick") ~= WatchFrameQuestPOI_OnClick then
                button:SetScript("OnClick", WatchFrameQuestPOI_OnClick)
            end
        end
    end
end

local hookedQuestMapSelect
local function bindCustomQuestMapSelection()
    if hookedQuestMapSelect or type(QM_SelectQuestEntry) ~= "function" then return end
    local original = QM_SelectQuestEntry
    QM_SelectQuestEntry = function(logIndex, questID)
        original(logIndex, questID)
        local resolved = tonumber(questID) or 0
        if resolved <= 0 and type(WORLDMAP_SETTINGS) == "table" then
            resolved = tonumber(WORLDMAP_SETTINGS.selectedQuestId) or 0
        end
        if resolved <= 0 then resolved = questIDForIndex(logIndex) end
        if resolved > 0 then selectQuest(resolved, "eunoia-map", false) end
    end
    hookedQuestMapSelect = true
end

driver:HookScript("OnUpdate", function(_, elapsed)
    poiBindPoll = poiBindPoll - elapsed
    if poiBindPoll <= 0 then
        poiBindPoll = 0.50
        bindCustomQuestMapSelection()
        bindTrackerPoiClicks()
    end
end)

local parent = _G.WarcraftXLOptionsPanel
if not parent then
    parent = CreateFrame("Frame", "WarcraftXLOptionsPanel", UIParent)
    parent.name = "WarcraftXL"
    InterfaceOptions_AddCategory(parent)
end
local options = CreateFrame("Frame", "QuestMarkerOptionsPanel", UIParent)
options.name, options.parent = "Objective Tracker", "WarcraftXL"
local title = options:CreateFontString(nil, "ARTWORK", "GameFontNormalLarge")
title:SetPoint("TOPLEFT", 16, -16)
title:SetText("WXL UI Tracker")
local description = options:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
description:SetPoint("TOPLEFT", title, "BOTTOMLEFT", 0, -6)
description:SetText("Configure quest navigation markers and the off-screen direction indicator.")

-- The 3.3.5a Interface Options addon pane is narrower and shorter than the
-- retail settings panel. Keep the heading fixed and place every control in a
-- single-column scroll child so additional navigation settings never extend
-- beyond the panel or its bottom buttons.
local scrollFrame = CreateFrame("ScrollFrame", "QuestMarkerOptionsScrollFrame",
    options, "UIPanelScrollFrameTemplate")
scrollFrame:SetPoint("TOPLEFT", options, "TOPLEFT", 8, -58)
scrollFrame:SetPoint("BOTTOMRIGHT", options, "BOTTOMRIGHT", -28, 8)

local scrollChild = CreateFrame("Frame", "QuestMarkerOptionsScrollChild", scrollFrame)
scrollChild:SetWidth(300)
scrollChild:SetHeight(730)
scrollFrame:SetScrollChild(scrollChild)
scrollFrame:EnableMouseWheel(true)
scrollFrame:SetScript("OnMouseWheel", function(self, delta)
    local maximum = self:GetVerticalScrollRange() or 0
    local target = self:GetVerticalScroll() - (delta * 40)
    self:SetVerticalScroll(math.max(0, math.min(maximum, target)))
end)

local refreshControls = {}
local function refreshNavigation()
    normalizeSettings()
    applySelectedMarker()
    updateTrackerDiamond()
    updateDistanceText()
end

local function addCheckbox(name, label, key, x, y)
    local button = CreateFrame("CheckButton", name, scrollChild,
        "InterfaceOptionsCheckButtonTemplate")
    button:SetPoint("TOPLEFT", scrollChild, "TOPLEFT", x, y)
    local text = scrollChild:CreateFontString(nil, "ARTWORK", "GameFontHighlight")
    text:SetPoint("LEFT", button, "RIGHT", 0, 1)
    text:SetText(label)
    button:SetScript("OnClick", function(self)
        QuestMarkerDB[key] = self:GetChecked() and true or false
        refreshNavigation()
    end)
    table.insert(refreshControls, function()
        button:SetChecked(QuestMarkerDB[key] and true or false)
    end)
    return button
end

local function addSlider(name, label, key, x, y, minimum, maximum, step, formatter)
    local slider = CreateFrame("Slider", name, scrollChild, "OptionsSliderTemplate")
    slider:SetPoint("TOPLEFT", scrollChild, "TOPLEFT", x, y)
    slider:SetWidth(220)
    slider:SetMinMaxValues(minimum, maximum)
    slider:SetValueStep(step)
    if _G[name .. "Low"] then _G[name .. "Low"]:SetText(tostring(minimum)) end
    if _G[name .. "High"] then _G[name .. "High"]:SetText(tostring(maximum)) end
    if _G[name .. "Text"] then _G[name .. "Text"]:SetText(label) end
    local valueText = scrollChild:CreateFontString(nil, "ARTWORK", "GameFontHighlightSmall")
    valueText:SetPoint("LEFT", slider, "RIGHT", 8, 0)
    slider:SetScript("OnValueChanged", function(self, value)
        if self.refreshing then return end
        value = math.floor((value / step) + 0.5) * step
        QuestMarkerDB[key] = value
        valueText:SetText(formatter(value))
        refreshNavigation()
    end)
    table.insert(refreshControls, function()
        slider.refreshing = true
        slider:SetValue(QuestMarkerDB[key])
        slider.refreshing = false
        valueText:SetText(formatter(QuestMarkerDB[key]))
    end)
    return slider
end

local distanceUnitOptions = {
    { value = "yards",  text = "Yards (yd)" },
    { value = "meters", text = "Metres (m)" },
    { value = "metric", text = "Metric (m / km)" },
}
local distanceUnitLabel = scrollChild:CreateFontString(nil, "ARTWORK", "GameFontHighlight")
distanceUnitLabel:SetPoint("TOPLEFT", scrollChild, "TOPLEFT", 24, -163)
distanceUnitLabel:SetText("Distance units")
local distanceUnitDropdown = CreateFrame("Frame", "QuestMarkerDistanceUnitDropdown",
    scrollChild, "UIDropDownMenuTemplate")
distanceUnitDropdown:SetPoint("TOPLEFT", scrollChild, "TOPLEFT", 4, -178)
UIDropDownMenu_SetWidth(distanceUnitDropdown, 150)
UIDropDownMenu_Initialize(distanceUnitDropdown, function()
    for _, option in ipairs(distanceUnitOptions) do
        local value, text = option.value, option.text
        local info = UIDropDownMenu_CreateInfo()
        info.text = text
        info.value = value
        info.checked = QuestMarkerDB.distanceUnit == value
        info.func = function()
            QuestMarkerDB.distanceUnit = value
            UIDropDownMenu_SetSelectedValue(distanceUnitDropdown, value)
            UIDropDownMenu_SetText(distanceUnitDropdown, text)
            refreshNavigation()
        end
        UIDropDownMenu_AddButton(info)
    end
end)
table.insert(refreshControls, function()
    local selectedText = distanceUnitOptions[1].text
    for _, option in ipairs(distanceUnitOptions) do
        if option.value == QuestMarkerDB.distanceUnit then selectedText = option.text; break end
    end
    UIDropDownMenu_SetSelectedValue(distanceUnitDropdown, QuestMarkerDB.distanceUnit)
    UIDropDownMenu_SetText(distanceUnitDropdown, selectedText)
end)

addCheckbox("QuestMarkerEnabled", "Enable quest navigation", "enabled", 16, -8)
addCheckbox("QuestMarkerWorldVisible", "Show destination marker", "showMarker", 16, -38)
addCheckbox("QuestMarkerOffscreenVisible", "Show off-screen direction indicator", "showOffscreen", 16, -68)
addCheckbox("QuestMarkerTrackerVisible", "Show tracker diamond", "showTrackerDiamond", 16, -98)
addCheckbox("QuestMarkerDistanceVisible", "Show destination distance", "showDistance", 16, -128)

addSlider("QuestMarkerOpacity", "Marker opacity", "alpha", 24, -248,
    25, 255, 5, function(value) return string.format("%d", value) end)
addSlider("QuestMarkerWorldScale", "World marker size", "worldMarkerScale", 24, -313,
    0.5, 2, 0.05, function(value) return string.format("%d%%", value * 100) end)
addSlider("QuestMarkerTrackerScale", "Tracker diamond size", "trackerDiamondScale", 24, -378,
    0.5, 2, 0.05, function(value) return string.format("%d%%", value * 100) end)

local layoutTitle = scrollChild:CreateFontString(nil, "ARTWORK", "GameFontNormal")
layoutTitle:SetPoint("TOPLEFT", scrollChild, "TOPLEFT", 16, -430)
layoutTitle:SetText("Off-screen indicator layout")
addSlider("QuestMarkerOffscreenScale", "Indicator size", "offscreenScale", 24, -478,
    0.5, 2, 0.05, function(value) return string.format("%d%%", value * 100) end)
addSlider("QuestMarkerOffscreenX", "Horizontal offset", "offscreenOffsetX", 24, -543,
    -500, 500, 5, function(value) return string.format("%+d px", value) end)
addSlider("QuestMarkerOffscreenY", "Vertical offset", "offscreenOffsetY", 24, -608,
    -350, 350, 5, function(value) return string.format("%+d px", value) end)

local reset = CreateFrame("Button", nil, scrollChild, "UIPanelButtonTemplate")
reset:SetSize(180, 24)
reset:SetPoint("TOPLEFT", scrollChild, "TOPLEFT", 24, -675)
reset:SetText("Reset marker layout")
reset:SetScript("OnClick", function()
    QuestMarkerDB.alpha = 165
    QuestMarkerDB.worldMarkerScale = 1
    QuestMarkerDB.offscreenScale = 1
    QuestMarkerDB.trackerDiamondScale = 1
    QuestMarkerDB.offscreenOffsetX = 0
    QuestMarkerDB.offscreenOffsetY = 0
    for _, refresh in ipairs(refreshControls) do refresh() end
    refreshNavigation()
end)

local function refreshOptions()
    normalizeSettings()
    for _, refresh in ipairs(refreshControls) do refresh() end
end
options.refresh = refreshOptions
options:SetScript("OnShow", refreshOptions)
InterfaceOptions_AddCategory(options)

_G.SlashCmdList = _G.SlashCmdList or {}
SLASH_QUESTMARKER1, SLASH_QUESTMARKER2 = "/questmarker", "/qm"
SlashCmdList.QUESTMARKER = function(message)
    local command = string.lower((message or ""):match("^%s*(.-)%s*$"))
    if command == "debug" then
        local markerCount = 0
        local markerIDs = {}
        for questID in pairs(Tracker.cache) do
            markerCount = markerCount + 1
            table.insert(markerIDs, tonumber(questID) or 0)
        end
        table.sort(markerIDs)
        local customIndex, customQuest, mapQuest, logIndex, logQuest = readSelection()
        print(string.format(
            "|cff66ccffUI Tracker|r: upstream=v2 markers=%d selected=%d source=%s rendered=%d kills=%d batch=%s bridge=%s",
            markerCount, Tracker.selectedQuest, Tracker.selectedSource,
            Tracker.currentQuest, #Tracker.killEntries, tostring(Tracker.batch),
            tostring(native() ~= nil)))
        print(string.format(
            "|cff66ccffUI Tracker|r: cache=[%s] custom=%d/%d map=%d log=%d/%d",
            table.concat(markerIDs, ","), customIndex, customQuest, mapQuest,
            logIndex, logQuest))
        print(string.format(
            "|cff66ccffUI Tracker|r: state=%s enabled=%s marker=%s api=%s/%s",
            tostring(Tracker.renderState), tostring(QuestMarkerDB.enabled),
            tostring(QuestMarkerDB.showMarker), type(SetQuestMarker),
            type(SetTrackerDiamond)))
        local distX, distY, dist
        if GetDistInfo then distX, distY, dist = GetDistInfo() end
        print(string.format(
            "|cff66ccffUI Tracker|r: distance=%s/%s/%s shown=%s enabled=%s units=%s",
            tostring(distX), tostring(distY), tostring(dist),
            tostring(distanceFrame:IsShown()), tostring(QuestMarkerDB.showDistance),
            tostring(QuestMarkerDB.distanceUnit)))
    elseif command == "refresh" then
        requestSnapshotAt = GetTime()
        print("|cff66ccffUI Tracker|r: snapshot requested.")
    elseif command == "on" or command == "off" then
        QuestMarkerDB.enabled = command == "on"
        applySelectedMarker()
        print("|cff66ccffUI Tracker|r: " .. (QuestMarkerDB.enabled and "enabled." or "disabled."))
    elseif InterfaceOptionsFrame_OpenToCategory then
        InterfaceOptionsFrame_OpenToCategory(options)
        InterfaceOptionsFrame_OpenToCategory(options)
    end
end

applySettings()
