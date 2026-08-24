# wxl-quest-marker

WarcraftXL ABI 1.1 port of the world-space quest navigation work from `bozo-1/WXL-UI-Tracker` for an
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

The quest-navigation concept and original implementation are credited to `bozo-1/WXL-UI-Tracker`.
The code in this repository is the WarcraftXL/AzerothCore port and retains that provenance.

## License

GPL-3.0-or-later. See `LICENSE`.
