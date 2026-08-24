// SelCirclePoc.hpp — proof-of-concept: a native selection circle for EVERY unit,
// plus quest-mob highlight (native circles on the selected quest's kill/interaction
// creatures, driven by the opcode-200 quest tracker).
//
// Reverse-engineering result (cross-validated across all.sym, binana 3.3.5a symbols/headers,
// the call trees and the cmpgraph 335 call graph, plus Ghidra passes 11/12/13):
//   * The selection circle texture (Textures\UnitSelectTexture.blp, global DAT_00ca12c4) is drawn
//     per frame by ProjectTex2d (0x7e4370), called by the vtable-dispatched RenderTargetSelection
//     family (CGUnit_C 0x725980 / pet 0x725bf0 / autotrack 0x7275c0 / CGCorpse 0x7062f0).
//   * Each renderer runs: RsPush -> SetCircleRenderStates (0x744eb0; binds the circle texture into
//     Gx render state 0x15 + fade tex into 0x16) -> ProjectTex2d -> RsPop.
//   * The selection-circle draw is uniquely (blend == 0x200122 && style == 1).
//   * The earlier hook target 0x6865b0 is CGxDevice__RsInit (minimap render-state init) — never on
//     the circle path, which is why only one circle appeared.
//
// The module hooks ProjectTex2d and, after the native draw, (a) clones the target's circle with a
// lateral offset, and (b) iterates a GetObjectByGuid POST-hook unit GUID cache (the ObjectManager
// hash walk was DISPROVEN for this client) and draws a native selection circle at every matching
// unit's own position/radius — all through the identical native draw path while render states
// 0x15/0x16 are still live. In quest mode (opcode-200 entries set) the circles are driven from the
// OnWorldRenderEnd event so they render even with no unit selected; only the selected quest's
// kill/interaction creatures are highlighted.
#pragma once

#include <cstdint>

namespace wxl_quest_marker
{
    /// Installs the ProjectTex2d + RenderTargetSelection detours through the Hub ABI.
    bool InstallQuestHighlight();
    void SetQuestHighlightEntries(const uint32_t* entries, uint32_t count);
    void ClearQuestHighlightEntries();
}
