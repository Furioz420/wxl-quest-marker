# wxl-quest-marker

WarcraftXL ABI 1.1 port of the world-space quest navigation work from [bozo-1/WXL-UI-Tracker](https://github.com/bozo-1/WXL-UI-Tracker) for an
AzerothCore-backed client.

The client renders selected-quest objectives, turn-ins, objective target circles, and corpse
navigation. The server owns eligibility, coordinates, creature resolution, and corpse state.

## Protocol

- `0x051F`: client request (`101` exact quest, `102` snapshot, `103` objective entries);
- `0x0520`: objective or turn-in marker;
- `0x0536`: selected-quest creature entries;
- `0x0537`: corpse position.

The current AzerothCore reference is in `server/azerothcore/wxl_quest_marker.cpp`. It matches the
native v1.1 opcode contract; it is reference integration code and is never installed by the Hub.

## Client data

The recreated addon is under `client/Interface/AddOns/QuestMarker`. It selects a quest and presents
tracker state while the server remains authoritative. Client-data files must be reviewed and
deployed through the project's client-data pipeline.

The Hub release contains only:

- `wxl-quest-marker.dll`;
- `wxl-quest-marker.cfg`.

## Requirements

- WarcraftXL Core ABI 1.1 with FrameScript and network services;
- `wxl-runtime` 1.1.0 or newer;
- the matching server and client-data prerequisites above.

Set `WXL_QUEST_MARKER=0` in `wxl-quest-marker.cfg` to disable the native extension.

## Attribution

The quest-navigation concept and original implementation are credited to [bozo-1/WXL-UI-Tracker](https://github.com/bozo-1/WXL-UI-Tracker).
The code in this repository is the WarcraftXL/AzerothCore port and retains that provenance. Furioz is credited for the local v1.1 integration commits; preserve original source and asset notices.

## License

GPL-3.0-or-later. See `LICENSE`.

## Integration and release checks

Build the Win32 DLL against the matching core and Runtime 1.1 APIs. The repository release workflow packages the DLL and config only; the server reference and client interface/texture payload must be reviewed and deployed separately. The integrated Eunoia client now uses a built-in `FrameXML/FrameNew/WarcraftXL/QuestMarker/QuestMarker.lua` path. This standalone repository still contains the earlier addon loader and addon payload. Do not deploy both UI implementations together; reconcile the native bootstrap, FrameNew payload, and client manifest before declaring this repository release-ready.

With the matching server installed, test selected-objective, turn-in, target-circle, and corpse markers; compare server coordinates and client world placement, then inspect both logs. Keep prior DLL/config, server integration, and client assets for rollback. `main` currently auto-publishes against moving upstream `v1.1`, so pin and test that core first.
