# WarcraftXL quest marker

This module renders a world-space marker for the selected quest and synchronizes
marker positions from AzerothCore through WXL's raw opcode registry.

## Client

The files under `src/` are discovered automatically by the root WXL CMake
configuration. No module-specific CMake hook is required.

The module registers `Quest Marker` under the dedicated Interface Options -> WXL
tab. Marker enablement and opacity are stored as client CVars. Future client
features such as Ping can register their own panel in the same WXL tab.

The preferred texture path is:

```
textures\questmarker\diamond.blp
```

If that file is not present in the client MPQs, WXL creates the same diamond as
a procedural D3D9 texture. The module therefore works without a Patch-Z asset.

## AzerothCore

The portable server script lives at:

```
server/azerothcore/wxl_quest_marker.cpp
```

Copy it into `src/server/scripts/Custom/`, declare and call
`AddSC_wxl_quest_marker()` from `custom_script_loader.cpp`, and install the reusable
registry/integration files under `scripts/wxl-opcodes/server/azerothcore`.

The client requests a snapshot with CMSG `0x051F`; the server answers with SMSG
`0x0102`. Quest coordinates come from AzerothCore's in-memory `quest_poi` and
`quest_poi_points` data. Incomplete quests prefer objective POIs; completed
quests prefer the `ObjectiveIndex = -1` return POI. Only POIs on the player's
current map are sent.

No SQL changes are required when the world database already contains quest POI
data and `QuestPOI.Enabled = 1`.

## Smoke test

1. Log in with at least one incomplete quest for the current map.
2. Select different quests in the quest log or tracker and confirm the marker
   switches without `/reload`.
3. Complete or abandon a quest and confirm its old marker disappears.
4. Change maps and confirm markers from the previous map are cleared.
5. Use `/qma 0` through `/qma 255` to test marker opacity.
6. Open Interface Options -> WXL -> Quest Marker and verify
   enable/disable, opacity, and Defaults.
