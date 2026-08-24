// SelCirclePoc.cpp — proof-of-concept: a native selection circle for EVERY unit,
// plus quest-mob highlight (native circles on the selected quest's kill/interaction
// creatures, driven by the same opcode-200 quest tracker the kill-highlight module uses).
//
// Cross-source reverse-engineering (all.sym + binana 3.3.5a symbols/headers + CALL TREES +
// cmpgraph call graphs + Ghidra passes 11/12/13) — the definitive render path:
//
//   * World init: CGObject_C__Initialize (0x7460c0) registers the "ObjectSelectionCircle"
//     CVar (CVar__Register, 0x767fc0; it is a CVar, NOT a name-keyed object factory — an earlier
//     theory was wrong), loads Textures\UnitSelectTexture.blp into DAT_00ca12c4, fills a 12-slot
//     model array (FUN_007435e0 -> CM2Scene__CreateModel) and registers the ScaleUpdateHandler
//     mirror handler (ClntObjMgrSetTypeMirrorHandler, 0x4d5ba0).
//   * Per frame, the circle is drawn by the vtable-dispatched RenderTargetSelection family:
//       CGUnit_C__RenderTargetSelection  (0x725980, vtable 0xa32734/0xa34dfc)
//       CGUnit_C__RenderPetTargetSelection (0x725bf0)
//       CGUnit_C__RenderAutoTrackCursor   (0x7275c0)
//       CGCorpse_C__RenderTargetSelection (0x7062f0)
//     each running:
//       RsPush (0x409670)
//       CGObject_C__SetCircleRenderStates (0x744eb0)  -> binds the circle texture into Gx render
//           state 0x15 and ProjectTex2dGetAddFadeTex into 0x16, dirties the states
//       ProjectTex2d (0x7e4370)                       -> the ACTUAL draw (ProjectTex2dDraw 0x7e3e80),
//           called as (bounds6, color, rotMatrix, DAT_009e2ec4, blend, style, DAT_009f98d8)
//       RsPop (0x685fb0)
//   * The selection-circle draw is uniquely identified by blend==0x200122 && style==1
//     (unit/pet/autotrack; corpse uses style 0, world footprints style 5, other effects 0x220122).
//
// WHY THE EARLIER POC SHOWED ONE CIRCLE: it hooked 0x6865b0, which the call graph names
// CGxDevice__RsInit — render-state initialisation called by CGMinimapFrame_RenderInsideTexture,
// never on the circle path. The hook never fired.
//
// THE PER-UNIT FEATURE: the unit GUID cache is refreshed EVERY FRAME from the engine's own
// visible-objects list via ClntObjMgrEnumVisibleObjects (0x4d4b30, all.sym; cdecl int(cb,param),
// callback receives guidLo/guidHi per visible object, return 0 stops). That list reaches
// UNTARGETED quest mobs — the earlier GetObjectByGuid POST hook (0x4d4db0) only fired on
// on-demand resolves (targeting/tooltips), so nearby quest creatures never entered the cache
// (that is why only the selection ring ever rendered). A POST hook on GetObjectByGuid still
// supplements the cache mid-frame. The type gate mirrors the native `m_obj->m_type & mask`
// indirection (CGObject_C::m_obj at +0x08 is a POINTER; CGObjectData::m_type at m_obj+0x08;
// reading obj+0x08 directly tested the heap pointer's low bits — garbage that admitted players,
// HIER_TYPE_PLAYER=0x19, entry 0) and excludes players. The hook on ProjectTex2d (0x7e4370) runs
// the native target draw, then — while render states 0x15/0x16 are still live inside the caller's
// RsPush/RsPop window — iterates that cache and re-invokes ProjectTex2d once per nearby unit with
// that unit's own bounds (position + the radius field the native renderer itself reads at
// unit+0xB0C). Every unit gets a native selection circle through the identical draw path.
// Runs at most once per frame (OnFrame-reset gate), capped by count and player distance.
//
// THE QUEST-HIGHLIGHT FEATURE: when a quest with kill/interaction objectives is selected, the
// module listens on the same custom-packet opcode 200 the kill-highlight module consumes (quest
// selection requests go out as opcode 103, owned by the quest-marker module; the server answers
// with opcode 200 carrying up to 4 creature entry IDs). Those entries are stored in a POD array
// and circles are drawn ONLY on units whose entry — read native-first from
// CGObjectData::m_entryID (m_obj+0x0C, binana object.h), falling back to the GUID's creature
// entry bits 24-47 (MAKE_NEW_GUID) — is in the quest set.
//
// WHY THE QUEST CIRCLES COME FROM THE HOOK, NOT A RAW M2-BATCH PASS: the OnM2BatchDraw vehicle
// (raw D3D9 quads drawn inside the engine's M2 pass with kill-highlight's state resets) was
// DISPROVEN — it CORRUPTS the engine's own M2 pass: creatures turn invisible and the quads
// render as "photos of objects glued to my screen" (raw-state surgery mid-batch is exactly what
// the native M2 pass does not expect; see ENGINE_RESEARCH.md). The quest circles instead ride
// the SAME proven vehicle as the per-unit sweep: the ProjectTex2d hook (0x7e4370), which fires
// inside the native renderer's own RsPush/RsPop window where the circle texture (Gx render
// states 0x15/0x16), camera and transforms are ALREADY live — the identical context in which
// the native selection circles render. When a quest is selected but NO unit is targeted,
// the engine still calls CGUnit_C::RenderAutoTrackCursor (0x7275c0) once per visible unit every
// frame (measured ~21 calls/frame) — the autotrack vehicle. Its INTERNAL gate (unit record ==
// tracked-objective DAT_00ca1238/3c and tracker state DAT_00ca11f4 ∈ {4,10}) blocks the native
// draw for quests tracked via our custom opcode-200 system (those client globals are never
// populated), so it never reaches ProjectTex2d on its own — but its INVOCATION is the per-frame
// tick: the HkRenderAutoTrack hook runs the quest sweep from inside it (native world-pass
// context, Gx camera live, recipe args synthesized from the DAT globals + BuildSelectionRotMatrix),
// giving every quest-matching unit its own native circle with nothing selected.
// With no quest entries the sweep stays off and the client's native selected-target circle remains
// untouched.
//
// Copyright (C) 2026 WarcraftXL

#include "QuestHighlight.hpp"
#include "ExtensionApi.hpp"

#include "engine/events/Event.hpp"
#include "game/Binding.hpp"
#include "game/World.hpp"
#include "offsets/game/Unit.hpp"

#include <cstdint>
#include <Windows.h>

namespace wxl_quest_marker
{
    namespace world = wxl::game::world;
    namespace off   = wxl::offsets::game::unit;

    // ---------------------------------------------------------------------------
    // Reverse-engineered addresses (Wow.exe 3.3.5a), cross-validated against all.sym,
    // binana 3.3.5a symbols, the call trees and the cmpgraph 335 call graph.
    // ---------------------------------------------------------------------------
    constexpr uintptr_t kProjectTex2d      = 0x007e4370; // ProjectTex2d: the actual circle draw
    constexpr uintptr_t kRenderAutoTrackCursor = 0x007275C0; // CGUnit_C_RenderAutoTrackCursor (no-selection quest trigger)
    constexpr uintptr_t kRenderTargetSelection = 0x00725980; // exact native per-unit circle renderer
    // ClntObjMgrEnumVisibleObjects: the engine's per-frame visible-objects enumerator.
    // cdecl int(cb, param); walks the TLS ClntObjMgr's visible list (head at
    // [mgr+8]+0xAC, next via [mgr+8]+0xA4) and calls cb(guidLo, guidHi, param) for each;
    // return 0 stops the walk. all.sym end=0x4D4BA5, plain `ret` -> caller cleans 8.
    constexpr uintptr_t kEnumVisibleObjects = 0x004D4B30;

    // No-selection sweep trigger (HkRenderAutoTrack): the autotrack caller does NOT hand us
    // the recipe args (RenderAutoTrackCursor builds c4/c7/color/matrix internally), so the
    // sweep's recipe args are RE-SYNTHESIZED from the same sources the native path uses:
    // the c4/c7 VALUES at the DAT globals (0.5f / 0.4f, cross-validated in all.sym; see
    // ENGINE_RESEARCH.md) and a camera-facing rotation matrix via
    // CGObject_C::BuildSelectionRotMatrix (0x744150). Despite its class-qualified name,
    // the Wrath binary exposes this as a two-argument __stdcall helper:
    //   BuildSelectionRotMatrix(float matrix[16], const float worldPos[3])
    // Both arguments are on the stack and the function returns with `ret 8`. Every
    // projected circle therefore needs a matrix built from that circle's own position.
    constexpr uintptr_t kBuildSelectionRotMatrix = 0x00744150;
    constexpr uintptr_t kDatC4 = 0x009E2EC4; // ProjectTex2d c4 value source (= 0.5f)
    constexpr uintptr_t kDatC7 = 0x009F98D8; // ProjectTex2d c7 value source (= 0.4f)

    // The native circle recipe the autotrack caller runs BEFORE ProjectTex2d (decompile
    // pass 11, FUN_007275c0: FUN_00409670(); iVar3 = FUN_00744eb0(); if (iVar3 != 0)
    // FUN_007e4370(...); FUN_00685fb0();). SetCircleRenderStates (0x744EB0) loads the
    // circle texture (FUN_004b6cb0(DAT_00ca12c4,0,0)) and binds it into Gx state 0x15
    // (FUN_00685f50) — the bind ProjectTex2d's internal draw (FUN_007e3e80) CONSUMES but
    // does NOT perform (it only binds the GUID into 0x16). The selected-mob hook path
    // inherits that bind from the native caller; the autotrack path (gate blocked) must
    // run the full recipe itself or stage 0x15 holds the world pass's arbitrary texture
    // -> solid black shape (observed).
    constexpr uintptr_t kRsPush               = 0x00409670; // Gx render-state array push
    constexpr uintptr_t kSetCircleRenderStates = 0x00744EB0; // bind circle texture -> state 0x15
    constexpr uintptr_t kRsPop                = 0x00685FB0; // Gx render-state array pop
    // The Gx render-state manager singleton (DAT_00C5DF88). RsPush/RsPop are __thiscall
    // members of it: every one of the 85/87 native call sites does `mov ecx,[0x00C5DF88]`
    // before the call (KB §1.3). A plain cdecl no-arg declaration CRASHES the client
    // (null-`this` -> write through ECX=0; observed 2026-08-02 at RsPop+0xB and 2026-08-03
    // at RsPush+0x55 = 0x4096C5, ERROR #132 both times). Emulate with __fastcall and pass
    // gx = *(void**)0x00C5DF88. The singleton is NULL during load screens — gate on it.
    constexpr uintptr_t kGxSingleton = 0x00C5DF88;

    using RsPushFn = void(__fastcall*)(void* gx);
    using SetCircleRenderStatesFn = int(__cdecl*)(); // nonzero = circle texture ready
    using RsPopFn = void(__fastcall*)(void* gx);
    // Gx render-state setter for the pointer-type slots (like state 0x15 = texture).
    // void __thiscall CGxDevice::RsSet_pointer_to_void(int state, void* value).
    constexpr uintptr_t kRsSetPointer = 0x00685F50;
    using RsSetPointerFn = void(__fastcall*)(void* gx, void* /*edx*/, int state, void* value);
    RsPushFn g_origRsPush = nullptr;
    SetCircleRenderStatesFn g_origSetCircleRenderStates = nullptr;
    RsPopFn g_origRsPop = nullptr;
    RsSetPointerFn g_origRsSetPointer = nullptr;

    // Engine texture system: the native circle loads its BLP through
    // CTexture::CreateFromFile (0x4B8A50, cdecl factory: path, flags, param3 -> CTexture*)
    // and resolves the GxTex wrapper via TextureGetGxTex (0x4B6CB0, cdecl: tex, mode, out;
    // all.sym `TextureGetGxTex 004B6CB0 f`). SetCircleRenderStates (0x744EB0) does exactly:
    //   gxTex = TextureGetGxTex(DAT_00ca12c4, 0, 0);  RsSet_pointer_to_void(0x15, gxTex);
    // ProjectTex2d's internal draw then samples state 0x15's GxTex WRAPPER — NOT a raw
    // IDirect3DTexture9* (RsSet_pointer_to_void reads value+0x2c/+0x5a and calls vtable+0
    // on it, which is why the previous raw-D3D swap never changed the rings). The quest
    // ring loads the custom BLP through the same engine path so we get the exact wrapper
    // type state 0x15 accepts.
    constexpr uintptr_t kCreateFromFile = 0x004B8A50;
    using CreateFromFileFn = void*(__cdecl*)(const char* path, uint32_t flags, uint32_t param3);
    constexpr uintptr_t kTextureGetGxTex = 0x004B6CB0;
    using TextureGetGxTexFn = void*(__cdecl*)(void* ctex, int mode, void* outGxTexPtr);
    // The native circle texture CTexture* global (SetCircleRenderStates passes it to
    // TextureGetGxTex) — used as the restore fallback for the state-0x15 swap.
    constexpr uintptr_t kNativeCircleTexture = 0x00CA12C4;
    CreateFromFileFn g_origCreateFromFile = nullptr;
    TextureGetGxTexFn g_origTextureGetGxTex = nullptr;

    // Quest-ring custom texture (engine-owned): the CTexture* is kept referenced so the
    // engine doesn't evict it; g_questGxTex is the wrapper bound into state 0x15.
    void* g_questCTexture = nullptr;
    void* g_questGxTex = nullptr;
    // Load backoff (GetTickCount ms): the engine texture loader must NOT run inside the
    // render sweep (observed fault — file I/O + texture alloc mid-draw), so the load is
    // triggered from OnFrame at Present; failures retry a few seconds later. Starts at 0
    // (immediate first attempt); OnDeviceReset resets it to re-attempt immediately.
    uint32_t g_questLoadRetryTick = 0;
    // State-0x15 swap guard for the quest ring: done once per frame inside the sweep.
    bool g_questTexSwapDone = false;
    // Saved native GxTex (the engine's selection-circle wrapper that was bound into state
    // 0x15 before the quest sweep), restored after the quest ring sweep.
    void* g_savedNativeGxTex = nullptr;

    using BuildSelectionRotMatrixFn = void(__stdcall*)(float* matrix, const float* pos);
    BuildSelectionRotMatrixFn g_origBuildSelectionRotMatrix = nullptr;

    // Quest-ring custom texture path (deployed in patch-A.MPQ).
    constexpr const char* kQuestTexPath = "textures\\questmarker\\unitselecttexture-quest.blp";

    // ProjectTex2d arg 5/6 discriminator for the selection-circle draw.
    constexpr uint32_t kCircleBlend = 0x200122;
    constexpr uint32_t kCircleStyle = 1; // unit/pet/autotrack (corpse = 0, world footprint = 5)

    // ---------------------------------------------------------------------------
    // Quest-ring APPEARANCE knobs (applied in the sweep, only to quest-mob rings — the
    // native selection circle is untouched). The color handed to ProjectTex2d is a
    // D3DCOLOR with ALPHA in the HIGH byte: the native fade (RenderTargetSelection
    // decompile, gh_pass11 FUN_00725980) scales `color >> 0x18` by the fade factor and
    // splices it back via CONCAT13 — so byte order for RGB is irrelevant to the scaling.
    //   kQuestRingBrightness : 0..1 multiplier on each RGB byte (dim/color intensity).
    //   kQuestRingOpacity    : 0..1 multiplier on the ALPHA byte (translucency).
    //   kQuestRingColor      : nonzero forces a fixed color (ABGR, alpha in the HIGH
    //                          byte — same convention as g_circleColor / the native
    //                          fade: 0xFFRRGGBB order in memory is 0xAABBGGRR), replacing
    //                          the per-unit UnitSelectionColor entirely (0 = keep native).
    constexpr float    kQuestRingBrightness = 0.40f; // native ring reads as too bright with the custom BLP
    constexpr float    kQuestRingOpacity    = 0.85f; // slight translucency softens the ring
    constexpr uint32_t kQuestRingColor      = 0xFFFFC800;     // 0 = per-unit color (yellow for quest mobs)
    // Keep quest targets visually distinct from the native selected-target circle. The
    // earlier camera-dependent blink was caused by sharing one projection matrix across
    // multiple world positions, not by this state-0x15 texture substitution.
    constexpr bool     kUseCustomQuestRingTexture = true;

    // PoC knobs.
    constexpr float   kMaxUnitDist = 60.0f;   // per-unit circles: player distance cap (yards)
    constexpr uint32_t kMaxUnitCircles = 24;  // per-unit circles: hard cap per frame
    constexpr float   kMinRadius = 0.5f, kMaxRadius = 30.0f; // plausibility for unit radius


    // The native renderer sizes the circle from *(float*)(unit + 0xB0C) — the renderer decompile
    // reads param_1[0x2c3] (= 0xB0C) as the half-extent r and builds bounds {pos-r, pos+r}.
    constexpr size_t kUnitCircleRadiusField = 0xB0C;
    // Fallback hitbox radius, read raw: UnitObject::boundingRadius (kUnitBoundingRadiusField,
    // 0x838) exists only in the local dev wxl-core, NOT the public release — a struct member
    // access breaks builds against official wxl-core (C2039). Same raw-offset pattern as
    // kUnitCircleRadiusField above.
    constexpr size_t kUnitBoundingRadiusField = 0x838;

    // Death signal: the unit's predicted health. Lua's UnitHealth (Script_UnitHealth
    // 0x60EB60, cmpgraph 335/410) calls CGUnit_C_GetPredictedHealth (0x71C2C0, 28-byte
    // getter; SetPredictedHealth at 0x716050) — the client's own health field. A dead
    // creature has health 0; on respawn it is > 0. (The first attempt used
    // `*(byte*)(unit + 0x28C) & 0x10` from RenderTargetSelection's fade block, but that
    // block reads the PLAYER's death-time field at player+0x1934, not the unit's — the bit
    // never fires for a killed creature, DISPROVEN 2026-08-03.)
    constexpr uintptr_t kGetPredictedHealth = 0x0071C2C0;
    using GetPredictedHealthFn = int(__fastcall*)(void* unit); // __thiscall: unit in ECX
    GetPredictedHealthFn g_origGetPredictedHealth = nullptr;

    // Per-unit circle color: the native selection renderer calls the virtual at vtable+0x78
    // (`(**(code**)(*unit + 0x78))(&color)` in FUN_00725980). For CGUnit that is
    // CGUnit_C_GetSelectionHighlightColor (0x718AC0) -> CGGameUI_GetSelectionHighlightColor
    // (0x521BF0, the function behind Lua's UnitSelectionColor). thiscall emulation: unit in
    // ECX, out color on the stack; returns nonzero on success (base CGObject writes white).
    constexpr size_t kUnitColorVtableOffset = 0x78;
    using UnitColorFn = uint32_t(__fastcall*)(void* unit, void* edx, uint32_t* outColor);

    // Quest kill/interaction entries: at most 4 (the server's opcode-200 payload cap).
    constexpr uint32_t kMaxQuestEntries = 4;

    // ProjectTex2d signature: (bounds6, color, rotMatrix, c4, blend, style, c7) — plain cdecl.
    using ProjectTex2dFn = void(__cdecl*)(void* bounds, void* color, void* matrix,
                                          uint32_t c4, uint32_t blend, uint32_t style, uint32_t c7);
    ProjectTex2dFn g_origProjectTex2d = nullptr;

    // RenderAutoTrackCursor: __fastcall, ecx = unit.
    using RenderAutoTrackFn = void(__fastcall*)(void* unit);
    RenderAutoTrackFn g_origRenderAutoTrack = nullptr;
    using RenderTargetSelectionFn = void(__fastcall*)(void* unit);
    RenderTargetSelectionFn g_origRenderTargetSelection = nullptr;

    // True only while the quest path invokes the exact native target-circle renderer.
    // HkProjectTex2d uses it to substitute the quest texture/color for that one draw.
    bool g_renderingQuestCircle = false;


    // ClntObjMgrEnumVisibleObjects binding. The two "object" args the callback receives
    // are actually the low/high GUID dwords of each visible object (the native callback
    // at 0x4F6A40 repacks them and calls GetObjectByGuid with typeMask=1).
    using EnumVisibleCbFn = int(__cdecl*)(uint32_t guidLo, uint32_t guidHi, void* param);
    using EnumVisibleObjectsFn = int(__cdecl*)(EnumVisibleCbFn cb, void* param);
    EnumVisibleObjectsFn g_origEnumVisible = nullptr;

    // ---------------------------------------------------------------------------
    // Quest-highlight entries. POD array (no std::set) so the SEH-guarded per-unit
    // sweep can consult it without C++ objects needing unwinding (MSVC C2712).
    // Written from the Lua bridge (main thread), read on the render thread — same
    // arrangement as kill-highlight's SetEntries; no locking needed in 3.3.5.
    // ---------------------------------------------------------------------------
    uint32_t g_questEntries[kMaxQuestEntries] = {};
    uint32_t g_questEntryCount = 0;

    // Color captured from the last real native circle draw (D3DCOLOR, ABGR). FALLBACK for
    // the sweep when a unit's vtable+0x78 color getter is unavailable — the per-unit getter
    // (UnitSelectionColor) is now the primary source, which eliminated the login-time white
    // warm-up and the re-poisoning by a dying mob's faded white circle.
    uint32_t g_circleColor = 0xFFFFFFFF;

    // ---------------------------------------------------------------------------
    // Custom quest-ring texture loader. Loads the BLP through the ENGINE's own texture
    // system — CTexture::CreateFromFile (0x4B8A50, opens through the game's archive IO,
    // which WarcraftXL extends to the loose patch-A.MPQ folder) followed by
    // TextureGetGxTex (0x4B6CB0) with mode=1 (wait for the async read). This yields the
    // engine GxTex WRAPPER that Gx state 0x15 accepts — the exact type the native circle
    // binds (SetCircleRenderStates), unlike a raw IDirect3DTexture9* (which
    // RsSet_pointer_to_void dereferences as a wrapper: value+0x2c/+0x5a + vtable+0).
    // No manual BLP/DXT decoding needed: the engine handles the format natively.
    // The BLP is deployed by the extension package at
    // Data/Patch-W.MPQ/Textures/questmarker/unitselecttexture-quest.blp.
    // CTexture::CreateFromFile (0x4B8A50) is NOT plain cdecl: it takes an implicit EAX
    // register argument — a writable 260-byte filename buffer — that the callee zeroes on
    // entry (`mov byte ptr [edi],0` at +0x61, EDI=EAX). Verified against Wow.exe.clean
    // with a corrected section table (the earlier RVA mapping was wrong): the prologue is
    // `55 8B EC 51 53 56 57 6A 00 8B F8` (mov edi,eax) and the observed in-game fault
    // eip 0x4B8AB1 is exactly `C6 07 00`. A plain cdecl call leaves EAX = garbage → write
    // to an invalid address → ACCESS_VIOLATION (the definitive root cause, observed across
    // every load attempt in all contexts). The engine's own caller (CreateBlpTexture, call
    // site 0x4B8C40) sets EAX = GetFileName()'s 260-byte buffer before the call.
    //
    // The toolchain is clang-cl, which rejects __declspec(naked) (C2059), so the EAX-setting
    // thunk is emitted as RUNTIME MACHINE CODE instead — compiler-independent, no inline
    // asm, 12 bytes on an executable page:
    //     B8 <imm32>    mov eax, <g_questTexNameBuf>
    //     BA <imm32>    mov edx, <g_origCreateFromFile>
    //     FF E2         jmp edx
    // The 3 stack args (path, flags, param3) pass through untouched (cdecl) and the
    // callee's own prologue/ret handles the frame (tail jump, same as the naked version).
    static char g_questTexNameBuf[260];
    static CreateFromFileFn g_questCreateFromFileThunk = nullptr;

    static void BuildCreateFromFileEaxThunk()
    {
        if (g_questCreateFromFileThunk) return;
        uint8_t* page = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                               PAGE_EXECUTE_READWRITE);
        if (!page) return;
        uint8_t* p = page;
        *p++ = 0xB8; // mov eax, imm32
        { const uint32_t v = (uint32_t)(uintptr_t)g_questTexNameBuf; memcpy(p, &v, 4); } p += 4;
        *p++ = 0xBA; // mov edx, imm32
        // NOTE: the target address is baked in at build time. Correct because
        // g_origCreateFromFile is resolved ONCE in InstallModule before any frame fires
        // (the thunk is built lazily on the first load attempt). The thunk would go stale
        // if that pointer were ever re-resolved at runtime.
        { const uint32_t v = (uint32_t)(uintptr_t)g_origCreateFromFile; memcpy(p, &v, 4); } p += 4;
        *p++ = 0xFF; *p++ = 0xE2; // jmp edx
        FlushInstructionCache(GetCurrentProcess(), page, (SIZE_T)(p - page));
        g_questCreateFromFileThunk = (CreateFromFileFn)(void*)page;
    }

    static void LoadQuestRingTexture()
    {
        if (g_questGxTex || !g_origCreateFromFile || !g_origTextureGetGxTex) return;
        if (!g_questCreateFromFileThunk) BuildCreateFromFileEaxThunk();
        if (!g_questCreateFromFileThunk) return;
        void* ctex = nullptr;
        __try
        {
            ctex = g_questCreateFromFileThunk(kQuestTexPath, 0, 0);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
        if (!ctex) return;
        g_questCTexture = ctex;
        __try
        {
            g_questGxTex = g_origTextureGetGxTex(ctex, 1, nullptr); // mode 1 = wait async read
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return;
        }
    }

    static void FreeQuestRingTexture()
    {
        // The CTexture from CreateFromFile is engine-owned (blob-cached by path via
        // FindByName, so a later CreateFromFile with the same path returns the cached
        // blob) — we deliberately do NOT release it; on device reset we only drop our
        // cached wrapper so the (possibly recreated) GxTex is re-resolved.
        g_questGxTex = nullptr;
    }

    static uint32_t QuestCircleColor(uint32_t nativeColor)
    {
        uint32_t color = kQuestRingColor ? kQuestRingColor : nativeColor;
        if (kQuestRingBrightness < 1.0f || kQuestRingOpacity < 1.0f)
        {
            const uint32_t a = (uint32_t)((color >> 24) * kQuestRingOpacity) & 0xFFu;
            const uint32_t r = (uint32_t)(((color >> 16) & 0xFFu) * kQuestRingBrightness) & 0xFFu;
            const uint32_t g = (uint32_t)(((color >> 8) & 0xFFu) * kQuestRingBrightness) & 0xFFu;
            const uint32_t b = (uint32_t)((color & 0xFFu) * kQuestRingBrightness) & 0xFFu;
            color = (a << 24) | (r << 16) | (g << 8) | b;
        }
        return color;
    }

    // SetCircleRenderStates has already installed the native selection texture when this
    // runs from HkProjectTex2d. Substitute only state 0x15 for the duration of one exact
    // native quest-circle draw, then restore it before returning to the engine.
    static void BeginQuestTextureSwap()
    {
        if (!kUseCustomQuestRingTexture || !g_questGxTex || !g_origRsSetPointer ||
            g_questTexSwapDone)
            return;

        void* gx = *reinterpret_cast<void**>(kGxSingleton);
        if (!gx) return;

        void* nativeGxTex = nullptr;
        uint8_t* stateTable = *reinterpret_cast<uint8_t**>(
            static_cast<uint8_t*>(gx) + 0x28f4);
        if (stateTable)
            nativeGxTex = *reinterpret_cast<void**>(stateTable + 0x15 * 0x18);
        if (!nativeGxTex && g_origTextureGetGxTex)
        {
            void* nativeCtex = *reinterpret_cast<void**>(kNativeCircleTexture);
            if (nativeCtex)
                nativeGxTex = g_origTextureGetGxTex(nativeCtex, 0, nullptr);
        }
        if (!nativeGxTex) return;

        g_savedNativeGxTex = nativeGxTex;
        g_questTexSwapDone = true;
        g_origRsSetPointer(gx, nullptr, 0x15, g_questGxTex);
    }

    static void EndQuestTextureSwap()
    {
        if (g_questTexSwapDone && g_origRsSetPointer && g_savedNativeGxTex)
        {
            void* gx = *reinterpret_cast<void**>(kGxSingleton);
            if (gx) g_origRsSetPointer(gx, nullptr, 0x15, g_savedNativeGxTex);
        }
        g_questTexSwapDone = false;
        g_savedNativeGxTex = nullptr;
    }

    // OnWorldEnter subscriber: loads the quest-ring texture once at first world entry.
    static void OnWorldEnter(void* /*user*/, const void* /*args*/)
    {
        LoadQuestRingTexture();
    }

    // OnDeviceReset subscriber: re-resolve the GxTex wrapper after a device reset
    // (the engine may recreate its GPU textures).
    static void OnDeviceReset(void* /*user*/, const void* /*args*/)
    {
        FreeQuestRingTexture();
        // Re-attempt the full engine load from the next OnFrame (SEH-guarded there);
        // CreateFromFile is blob-cached by path so a re-create is cheap.
        g_questLoadRetryTick = 0;
    }

    // ---------------------------------------------------------------------------
    // Unit GUID cache. The ObjectManager hash walk (singleton 0x00AD3D64, buckets +0x20,
    // nodes {next,type,guid} @ +0/4/C) was DISPROVEN for this client: the real manager is
    // per-thread (TLS slot [0x00D439BC] via fs:[0x2C]), the slot array is at table+0x1C
    // with the mask at table+0x24, and the object's type mask lives at obj+0x08
    // (GetObjectByGuid/FindActiveObj disassembly). So instead of walking the hash we use
    // the proven kill-highlight mechanism: a POST hook on GetObjectByGuid (0x4D4DB0)
    // caches every unit GUID the client resolves (the client resolves all in-range units
    // for server updates / rendering). POD ring: written by the hook, read by the draw
    // paths on the render thread (3.3.5 single-thread model; a torn read just produces a
    // bogus guid that fails resolve and is skipped).
    // ---------------------------------------------------------------------------
    constexpr uint32_t kMaxCachedUnits = 128;
    uint64_t g_unitCache[kMaxCachedUnits] = {};
    uint32_t g_unitCacheCount = 0;
    uint32_t g_unitCacheWrite = 0; // FIFO ring index for eviction when full
    static void CacheUnitGuid(uint64_t guid)
    {
        if (!guid) return;
        for (uint32_t i = 0; i < g_unitCacheCount; ++i)
            if (g_unitCache[i] == guid) return; // dedupe
        if (g_unitCacheCount < kMaxCachedUnits)
            g_unitCache[g_unitCacheCount++] = guid; // slot written before count (torn-read safe)
        else
        {
            g_unitCache[g_unitCacheWrite] = guid;
            g_unitCacheWrite = (g_unitCacheWrite + 1) % kMaxCachedUnits;
        }
    }

    static void ClearUnitCache()
    {
        g_unitCacheCount = 0;
        g_unitCacheWrite = 0;
    }

    // Type check with the native indirection: CGObject_C::m_obj is a POINTER at
    // obj+0x08 (binana object.h), and the type flags live in the pointed-to
    // CGObjectData at m_obj+0x08. The client's own GetObjectByGuid body does exactly
    // `mov ecx,[obj+0x08]; test [ecx+0x08], mask` (0x4D4DB0), and the native
    // visible-objects callback at 0x4F6A40 does `mov edx,[obj+8]; mov ecx,[edx+8];
    // shr ecx,3; test cl,1` = m_obj->m_type & TYPE_UNIT. Reading *(uint32*)(obj+0x08)
    // directly tests the heap POINTER's low bits - garbage that admitted players
    // (HIER_TYPE_PLAYER=0x19, entry 0) into the cache, so every quest match failed.
    static bool IsUnitNotPlayer(void* obj)
    {
        if (!obj) return false;
        void* m_obj = *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + 0x08);
        if (!m_obj) return false;
        const uint32_t type = *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(m_obj) + 0x08);
        // TYPE_UNIT=0x08 set, TYPE_PLAYER=0x10 clear (binana object/type.h).
        return (type & 0x18) == 0x08;
    }

    // Entry extraction, native-first: CGObjectData::m_entryID sits at m_obj+0x0C
    // (binana object.h: m_guid+0, m_type+0x08, m_entryID+0x0C). Fall back to the
    // WotLK GUID packing (entry at bits 24-47) when the field is degenerate.
    static uint32_t EntryFromObject(void* obj, uint64_t guid)
    {
        if (obj)
        {
            void* m_obj = *reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + 0x08);
            if (m_obj)
            {
                const int32_t e = *reinterpret_cast<int32_t*>(
                    static_cast<uint8_t*>(m_obj) + 0x0C);
                if (e > 0) return static_cast<uint32_t>(e);
            }
        }
        return static_cast<uint32_t>((guid >> 24) & 0x00FFFFFF);
    }

    off::GetObjectFn g_origGetObject = nullptr;

    // POST hook: cache unit GUIDs as the client resolves objects. The type check now
    // mirrors the native `m_obj->m_type & mask` indirection (and excludes players).
    static void* __cdecl HkGetObjectByGuid(unsigned long long guid, unsigned typeMask,
                                           const char* tag, int flag)
    {
        void* obj = g_origGetObject(guid, typeMask, tag, flag);
        if (obj && IsUnitNotPlayer(obj))
            CacheUnitGuid(guid);
        return obj;
    }

    // EnumVisibleObjects callback: receives (guidLo, guidHi, param) for every object in
    // the engine's per-frame visible-objects list. Resolve + gate inside the walk — exactly
    // what the native callback at 0x4F6A40 does (it calls GetObjectByGuid, then checks
    // m_type & TYPE_UNIT) — so the 128-slot ring holds ONLY units: players, game objects
    // and corpses never evict the unit guids the quest path needs. g_origGetObject is the
    // trampoline to the original (no re-entry through our hook). Returns 1 to continue.
    static int __cdecl EnumVisibleCb(uint32_t guidLo, uint32_t guidHi, void* /*param*/)
    {
        const uint64_t guid = (static_cast<uint64_t>(guidHi) << 32) | guidLo;
        if (!guid) return 1;
        void* unit = g_origGetObject(guid, world::kTypeMaskUnit, "wxl", 0);
        if (unit && IsUnitNotPlayer(unit))
            CacheUnitGuid(guid);
        return 1;
    }

    // Per-frame cache refresh from the engine's own visible-objects list. This is the
    // source that reaches UNTARGETED quest mobs: the GetObjectByGuid hook only fires on
    // on-demand resolves (targeting, tooltips), so nearby quest creatures never flowed
    // through it - that is why only the selection ring ever rendered. Run once per
    // frame from OnWorldRenderEnd. SEH-guarded (POD only, MSVC C2712-safe).
    static void __cdecl RefreshVisibleUnitCache()
    {
        if (!g_origEnumVisible) return;
        __try
        {
            // Clear + refill: the cache becomes exactly this frame's visible objects,
            // plus whatever the GetObjectByGuid hook adds mid-frame (re-added by the
            // enum if still visible). Stale guids from despawned mobs drop out.
            ClearUnitCache();
            g_origEnumVisible(&EnumVisibleCb, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    // GUIDs already rendered through the native per-unit quest path this frame. The
    // engine normally calls RenderAutoTrackCursor once per visible unit, but this guard
    // prevents a duplicate decal if a unit is visited by more than one render list.
    uint64_t g_drawnQuestGuids[kMaxUnitCircles] = {};
    uint32_t g_drawnQuestGuidCount = 0;

    // ---------------------------------------------------------------------------
    // OnFrame subscriber: resets the once-per-frame sweep gate and retries the
    // quest-ring texture load at Present (outside the world-draw window).
    // ---------------------------------------------------------------------------
    static void OnFrame(void* /*user*/, const void* /*args*/)
    {
        g_drawnQuestGuidCount = 0;

        // Quest-ring texture load at a SAFE point (Present, outside the world-draw
        // window): the engine texture loader does file I/O + async alloc. The historical
        // faults were NOT the context — they were the missing EAX buffer arg on
        // CreateFromFile (see the runtime machine-code EAX thunk), now fixed. Attempt when quest
        // mode is active and the texture is missing; the retry tick starts at 0
        // (immediate first try) and backs off 3 s after each failed attempt.
        if (!g_questGxTex && g_origCreateFromFile && g_origTextureGetGxTex &&
            g_questEntryCount > 0)
        {
            const uint32_t tick = GetTickCount();
            if (tick >= g_questLoadRetryTick)
            {
                g_questLoadRetryTick = tick + 3000;
                LoadQuestRingTexture();
            }
        }
    }

    // ---------------------------------------------------------------------------
    // Per-unit sweep: iterate the client-resolved unit GUID cache (GetObjectByGuid POST
    // hook + per-frame visible-objects enum, see CacheUnitGuid / RefreshVisibleUnitCache)
    // and draw one native selection circle per matching unit at its own feet. Each circle
    // gets its own camera-facing matrix built from that unit's world position; reusing the
    // triggering unit's matrix makes the other projected quads cross the camera clip plane
    // as pitch changes, which presents as camera-dependent flickering. Runs while
    // render states 0x15/0x16 are live inside the native caller's
    // RsPush/RsPop window, so every circle rides the identical native path.
    //
    // QUEST MODE: only units whose entry is in the selected quest's kill/interaction set
    // get a circle (the filter lives HERE, not in a separate draw path — the
    // OnM2BatchDraw vehicle was DISPROVEN, see the header). With no quest entries the
    // sweep is disabled, leaving the client's native selected-target circle alone.
    // Pure-C + SEH: no C++ objects needing unwinding (MSVC C2712); entries are
    // snapshotted before __try.
    // ---------------------------------------------------------------------------
    static void __cdecl SweepUnitsDrawCircles(void* color, uint32_t c4,
                                              uint32_t blend, uint32_t style, uint32_t c7)
    {
        if (!g_origBuildSelectionRotMatrix) return;

        uint32_t drawn = 0;
        const uint64_t targetGuid = world::TargetGuid();
        const uint64_t playerGuid = world::ActivePlayerGuid();
        float plr[3] = { 0.0f, 0.0f, 0.0f };
        bool havePlr = false;
        if (playerGuid)
        {
            void* p = world::ResolveObject(playerGuid, world::kTypeMaskPlayer);
            if (p) { world::UnitPosition(p, plr); havePlr = true; }
        }

        // Quest-entry snapshot (POD copy OUTSIDE __try — C2712-safe reads only). The
        // Lua bridge writes the entry array FIRST and the count LAST, so copy entries
        // before reading the count: a mid-update read can never pair a bumped count
        // with stale entry slots.
        uint32_t questEntries[kMaxQuestEntries] = { 0, 0, 0, 0 };
        for (uint32_t qi = 0; qi < kMaxQuestEntries; ++qi) questEntries[qi] = g_questEntries[qi];
        const uint32_t questCount = g_questEntryCount;
        if (questCount == 0) return;

        __try
        {
            const uint32_t mask = world::kTypeMaskUnit;
            const float maxDistSq = kMaxUnitDist * kMaxUnitDist;
            const uint32_t cacheCount = g_unitCacheCount; // POD snapshot

            for (uint32_t ci = 0; ci < cacheCount && drawn < kMaxUnitCircles; ++ci)
            {
                const uint64_t guid = g_unitCache[ci];
                if (!guid || guid == targetGuid || guid == playerGuid) continue;
                void* unit = world::ResolveObject(guid, mask);
                if (!unit) continue;
                if (!IsUnitNotPlayer(unit)) continue; // mask 0x08 also admits players
                // Entry must be in the selected quest's kill/interaction set.
                {
                    const uint32_t entry = EntryFromObject(unit, guid);
                    bool match = false;
                    for (uint32_t qi = 0; qi < questCount; ++qi)
                        if (questEntries[qi] == entry) { match = true; break; }
                    if (!match) continue;
                }
                // Only reached for quest-matching units: before the
                // first quest-ring draw, save the native GxTex from state 0x15 and
                // override it with our custom quest-ring GxTex. SetCircleRenderStates
                // already bound the native circle texture into 0x15 (the hook path
                // inherits it; the autotrack path calls SetCircleRenderStates itself). We
                // save the value dword at [gx + 0x28f4] + 0x15*0x18 + 0 — the SAME slot
                // RsSet_pointer_to_void reads/writes (entry = stateTable + state*0x18; the
                // value is the first dword; IRsDirty pushes stateArray[0] as the previous
                // value; the earlier +4 read pointed at a different dword of the entry and
                // was wrong). Fallback: the native circle texture's own GxTex
                // (TextureGetGxTex(DAT_00ca12c4, 0, 0) — what SetCircleRenderStates
                // binds). Gated on a real match (not just quest-mode) so the render-state
                // machine is not churned when no quest mobs are in range; done once per
                // frame via g_questTexSwapDone.
                if (kUseCustomQuestRingTexture && questCount > 0 && g_questGxTex &&
                    g_origRsSetPointer && !g_questTexSwapDone)
                {
                    void* gx = *reinterpret_cast<void**>(kGxSingleton);
                    if (gx)
                    {
                        void* nativeGxTex = nullptr;
                        // Save the native GxTex from state 0x15's value slot (+0).
                        uint8_t* stateTable = *reinterpret_cast<uint8_t**>(
                            static_cast<uint8_t*>(gx) + 0x28f4);
                        if (stateTable)
                            nativeGxTex = *reinterpret_cast<void**>(
                                stateTable + 0x15 * 0x18);
                        // Fallback: the native circle texture's GxTex (what
                        // SetCircleRenderStates binds into 0x15 every frame).
                        if (!nativeGxTex && g_origTextureGetGxTex)
                        {
                            void* nativeCtex = *reinterpret_cast<void**>(kNativeCircleTexture);
                            if (nativeCtex)
                                nativeGxTex = g_origTextureGetGxTex(nativeCtex, 0, nullptr);
                        }
                        // Only swap if we have something to restore to — otherwise the
                        // native circle would inherit the quest texture.
                        if (nativeGxTex)
                        {
                            g_questTexSwapDone = true;
                            g_savedNativeGxTex = nativeGxTex;
                            g_origRsSetPointer(gx, nullptr, 0x15, g_questGxTex);
                        }
                    }
                }
                // Skip dead units: the quest ring vanishes the moment the mob dies and
                // returns when it respawns. Uses the client's own getter (what Lua
                // UnitHealth reads — dead = exactly 0, respawn > 0; a -1 "unknown" sentinel
                // would be an alive unit we must NOT hide, hence `== 0` not `<= 0`). A null
                // binding skips the check (fail-open).
                if (g_origGetPredictedHealth && g_origGetPredictedHealth(unit) == 0)
                    continue;
                auto* uo = static_cast<off::UnitObject*>(unit);
                const float* pos = uo->position;
                // The native renderer sizes the circle from unit+0xB0C; fall back
                // to the bounding radius if that field is degenerate. Read the
                // fallback RAW at 0x838: UnitObject::boundingRadius exists only in
                // the local dev wxl-core and is absent from the public release, so a
                // struct member access would break builds against official wxl-core.
                float r = *reinterpret_cast<float*>(
                    static_cast<uint8_t*>(unit) + kUnitCircleRadiusField);
                if (r < kMinRadius || r > kMaxRadius)
                    r = *reinterpret_cast<float*>(
                        static_cast<uint8_t*>(unit) + kUnitBoundingRadiusField);
                const bool posOk = pos[0] > -20000.0f && pos[0] < 20000.0f
                                && pos[1] > -20000.0f && pos[1] < 20000.0f;
                if (r >= kMinRadius && r <= kMaxRadius && posOk)
                {
                    const float dx = pos[0] - plr[0];
                    const float dy = pos[1] - plr[1];
                    const float dz = pos[2] - plr[2];
                    if (!havePlr || dx * dx + dy * dy + dz * dz <= maxDistSq)
                    {
                        // Per-unit deterministic color via the native vtable+0x78 virtual
                        // (CGUnit_C_GetSelectionHighlightColor -> UnitSelectionColor). This
                        // replaces the captured g_circleColor, which started white at login
                        // and was re-poisoned by a dying mob's faded white circle. Falls
                        // back to the caller's color if the getter is unavailable.
                        uint32_t unitColor = color
                            ? *reinterpret_cast<const uint32_t*>(color)
                            : 0xFFFFFFFF;
                        void** vt = *reinterpret_cast<void***>(unit);
                        if (vt)
                        {
                            const auto getColor = reinterpret_cast<UnitColorFn>(
                                vt[kUnitColorVtableOffset / sizeof(void*)]);
                            if (getColor)
                            {
                                uint32_t c = 0;
                                if (getColor(unit, nullptr, &c)) unitColor = c;
                            }
                        }
                        // Quest-ring appearance: force a fixed color and/or scale the
                        // per-unit color's RGB (brightness) and ALPHA byte (opacity).
                        // These appearance controls are quest-ring-only; the native
                        // selected-target circle never enters this sweep.
                        {
                            if (kQuestRingColor != 0)
                                unitColor = kQuestRingColor;
                            if (kQuestRingBrightness < 1.0f || kQuestRingOpacity < 1.0f)
                            {
                                const uint32_t a = (uint32_t)((unitColor >> 24) * kQuestRingOpacity) & 0xFFu;
                                const uint32_t r = (uint32_t)(((unitColor >> 16) & 0xFFu) * kQuestRingBrightness) & 0xFFu;
                                const uint32_t g = (uint32_t)(((unitColor >> 8) & 0xFFu) * kQuestRingBrightness) & 0xFFu;
                                const uint32_t b = (uint32_t)((unitColor & 0xFFu) * kQuestRingBrightness) & 0xFFu;
                                unitColor = (a << 24) | (r << 16) | (g << 8) | b;
                            }
                        }
                        float bounds[6] = { pos[0] - r, pos[1] - r, pos[2] - r,
                                            pos[0] + r, pos[1] + r, pos[2] + r };
                        float unitMatrix[16] = {};
                        g_origBuildSelectionRotMatrix(unitMatrix, pos);
                        g_origProjectTex2d(bounds, &unitColor, unitMatrix,
                                           c4, blend, style, c7);
                        ++drawn;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // Restore on exception too — the state 0x15 was overridden even if
            // the loop body faulted.
            if (g_questTexSwapDone && g_origRsSetPointer && g_savedNativeGxTex)
            {
                void* gx = *reinterpret_cast<void**>(kGxSingleton);
                if (gx) g_origRsSetPointer(gx, nullptr, 0x15, g_savedNativeGxTex);
            }
            g_questTexSwapDone = false;
            g_savedNativeGxTex = nullptr;
            return;
        }
        // Restore the native GxTex for the native circle path.
        if (g_questTexSwapDone && g_origRsSetPointer && g_savedNativeGxTex)
        {
            void* gx = *reinterpret_cast<void**>(kGxSingleton);
            if (gx) g_origRsSetPointer(gx, nullptr, 0x15, g_savedNativeGxTex);
        }
        g_questTexSwapDone = false;
        g_savedNativeGxTex = nullptr;
    }


    // ---------------------------------------------------------------------------
    // OnWorldLeave subscriber: clear the unit GUID cache so stale guids from a previous
    // zone never resolve/draw in the new world.
    // ---------------------------------------------------------------------------
    static void OnWorldLeave(void* /*user*/, const void* /*args*/)
    {
        ClearUnitCache();
        g_questEntryCount = 0;
        for (uint32_t& entry : g_questEntries) entry = 0;
    }

    // ---------------------------------------------------------------------------
    // OnWorldRenderEnd subscriber: refreshes the visible-objects unit cache once per
    // frame at the world -> UI boundary (terrain already drawn, UI not yet). This event
    // only FEEDS the cache — the quest circles themselves are drawn by the hook's per-unit
    // sweep (next frame's first circle draw, selection OR autotrack), which consumes this
    // cache. Fires after the world pass, so the refreshed cache feeds the NEXT frame's
    // sweep (1-frame staleness — the same build-here/draw-there cadence as kill-highlight).
    // ---------------------------------------------------------------------------
    static void OnWorldRenderEnd(void* /*user*/, const void* /*args*/)
    {
        // Every frame, refresh the unit cache from the engine's own visible-objects list.
        // This reaches UNTARGETED quest mobs (the GetObjectByGuid hook only fires on
        // on-demand resolves), so rings now render with no selection.
        RefreshVisibleUnitCache();
    }

    // ---------------------------------------------------------------------------
    // ProjectTex2d detour: run the native draw, then once per frame sweep units and
    // give each matching unit its own native selection circle.
    // ---------------------------------------------------------------------------
    static void __cdecl HkProjectTex2d(void* bounds, void* color, void* matrix,
                                       uint32_t c4, uint32_t blend, uint32_t style, uint32_t c7)
    {
        // The quest path now invokes the client's exact per-unit target renderer. Its
        // SetCircleRenderStates call has already established the native state; change only
        // this draw's texture and color, then restore state before the native renderer pops.
        if (g_renderingQuestCircle && blend == kCircleBlend &&
            style == kCircleStyle && bounds != nullptr)
        {
            uint32_t questColor = color
                ? QuestCircleColor(*reinterpret_cast<const uint32_t*>(color))
                : QuestCircleColor(g_circleColor);
            BeginQuestTextureSwap();
            g_origProjectTex2d(bounds, &questColor, matrix, c4, blend, style, c7);
            EndQuestTextureSwap();
            return;
        }

        g_origProjectTex2d(bounds, color, matrix, c4, blend, style, c7);

        // Only touch the unit selection circle (blend 0x200122, style 1).
        if (blend == kCircleBlend && style == kCircleStyle && bounds != nullptr)
        {
            // Keep a fallback color for units whose vtable+0x78 getter is unavailable
            // (ABGR D3DCOLOR). The sweep's primary per-unit color is the native virtual
            // (see SweepUnitsDrawCircles) — this capture is only a fallback now.
            if (color) g_circleColor = *reinterpret_cast<const uint32_t*>(color);

            // Objective rings are rendered per unit by HkRenderAutoTrack through the
            // client's exact CGUnit_C::RenderTargetSelection path. Do not sweep every
            // cached unit from this one projection callback: that synthetic path is
            // sensitive to camera pitch and render traversal timing.
        }
    }

    // ---------------------------------------------------------------------------
    // RenderAutoTrackCursor detour: the NO-SELECTION quest trigger. The engine calls it
    // once per visible unit every frame while a quest is selected. Its INTERNAL gate
    // (unit's object record == the client's tracked-objective DAT_00ca1238/3c AND tracker
    // state DAT_00ca11f4 ∈ {4,10}, decompile pass 11) blocks the draw for quests tracked
    // via our custom opcode-200 system, which never populates those client globals — so it
    // fires constantly yet never reaches ProjectTex2d on its own.
    //
    // We use its INVOCATION as the per-frame tick instead: when quest entries are set and
    // the sweep hasn't run this frame, run the quest sweep from inside it. The context is
    // the native world pass (per-unit quest-cursor render), where the Gx camera is live —
    // exactly what ProjectTex2dDraw needs (FUN_004f6650 camera source; ProjectTex2d itself
    // does its own RsPush and binds the circle texture). The recipe args are synthesized
    // from the same sources the native path uses: c4=0.5f/c7=0.4f (the DAT globals the
    // native renderer pushes), a per-unit color (the vtable+0x78 GetSelectionHighlightColor
    // virtual, fallback captured from the last real draw), and a camera-facing
    // BuildSelectionRotMatrix. Shares the once-per-frame
    // g_sweptThisFrame gate with the ProjectTex2d hook path (whichever fires first in a
    // frame wins — both are valid world-pass contexts).
    static void __fastcall HkRenderAutoTrack(void* unit)
    {
        g_origRenderAutoTrack(unit);

        // Render this callback's unit through the exact native target-selection path.
        // This keeps model-space positioning, projection bounds, receiver selection and
        // render traversal timing identical to a normal stable target circle.
        if (!unit || g_questEntryCount == 0 || !g_origRenderTargetSelection)
            return;

        __try
        {
            if (!IsUnitNotPlayer(unit)) return;

            auto* base = static_cast<off::ObjectBase*>(unit);
            if (!base->header) return;
            const uint64_t guid = base->header->guid;
            if (!guid || guid == world::ActivePlayerGuid() || guid == world::TargetGuid())
                return;

            const uint32_t entry = EntryFromObject(unit, guid);
            bool match = false;
            const uint32_t questCount = g_questEntryCount;
            for (uint32_t qi = 0; qi < questCount && qi < kMaxQuestEntries; ++qi)
            {
                if (g_questEntries[qi] == entry) { match = true; break; }
            }
            if (!match) return;
            if (g_origGetPredictedHealth && g_origGetPredictedHealth(unit) == 0)
                return;

            float playerPos[3] = {};
            const uint64_t playerGuid = world::ActivePlayerGuid();
            if (playerGuid)
            {
                void* player = world::ResolveObject(playerGuid, world::kTypeMaskPlayer);
                if (player)
                {
                    world::UnitPosition(player, playerPos);
                    auto* uo = static_cast<off::UnitObject*>(unit);
                    const float dx = uo->position[0] - playerPos[0];
                    const float dy = uo->position[1] - playerPos[1];
                    const float dz = uo->position[2] - playerPos[2];
                    if (dx * dx + dy * dy + dz * dz > kMaxUnitDist * kMaxUnitDist)
                        return;
                }
            }

            for (uint32_t i = 0; i < g_drawnQuestGuidCount; ++i)
                if (g_drawnQuestGuids[i] == guid) return;
            if (g_drawnQuestGuidCount < kMaxUnitCircles)
                g_drawnQuestGuids[g_drawnQuestGuidCount++] = guid;
            else
                return;

            g_renderingQuestCircle = true;
            g_origRenderTargetSelection(unit);
            g_renderingQuestCircle = false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_renderingQuestCircle = false;
            EndQuestTextureSwap();
        }
    }

    // ---------------------------------------------------------------------------
    #if 0
    // Quest-entry Lua bridge (port of kill-highlight's ScriptSetEntries). Pure-C
    // SEH reader for the Lua stack (MSVC C2712), then a POD store outside __try.
    // ---------------------------------------------------------------------------
    using LuaNumFn = double(__cdecl*)(void* state, int idx);

    static int ScriptSetQuestEntriesSafe(void* state, uint32_t* outCount, double* outRaw)
    {
        __try
        {
            if (!state) return 1;
            auto toNum = wxl::game::Native<LuaNumFn>(wxl::offsets::engine::lua::kLuaToNumber);
            auto isNum = wxl::game::Native<wxl::offsets::engine::lua::LuaIsNumberFn>(wxl::offsets::engine::lua::kLuaIsNumber);
            int a1n = isNum(state, 1);
            double r1 = toNum(state, 1);
            *outCount = static_cast<uint32_t>(r1);
            if (!a1n || *outCount > kMaxQuestEntries) { *outCount = 0; return 1; }
            outRaw[0] = r1;
            for (uint32_t i = 0; i < *outCount && i < kMaxQuestEntries; ++i)
                outRaw[1 + i] = toNum(state, 2 + static_cast<int>(i));
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 1;
        }
    }

    int __cdecl ScriptSetQuestEntries(void* state)
    {
        // NOTE: no FrameScriptGetContext() state-guard (same as kill-highlight's
        // ScriptSetEntries): read the stack, SEH-guard the access, apply.
        uint32_t count = 0;
        double raw[kMaxQuestEntries + 1] = {0.0, 0.0, 0.0, 0.0, 0.0};
        if (ScriptSetQuestEntriesSafe(state, &count, raw) != 0) return 0;

        uint32_t entries[kMaxQuestEntries] = {0};
        uint32_t realCount = 0;
        for (uint32_t i = 0; i < count && i < kMaxQuestEntries; ++i)
        {
            uint32_t e = static_cast<uint32_t>(raw[1 + i]);
            if (e > 0 && realCount < kMaxQuestEntries) entries[realCount++] = e;
        }

        // Write the array FIRST, the count LAST: the render-thread sweep snapshots the count,
        // so a torn update can never pair a bumped count with stale entry slots.
        for (uint32_t i = 0; i < kMaxQuestEntries; ++i) g_questEntries[i] = entries[i];
        g_questEntryCount = realCount;
        return 0;
    }

    int __cdecl ScriptClearQuestEntries(void* state)
    {
        (void)state;
        g_questEntryCount = 0;
        for (uint32_t i = 0; i < kMaxQuestEntries; ++i) g_questEntries[i] = 0;
        return 0;
    }

    // ---------------------------------------------------------------------------
    // Module installer + self-registration
    // ---------------------------------------------------------------------------
    void InstallModule()
    {
        const bool projOk =
            wxl::core::hook::Install("selcircle-poc::projecttex2d", kProjectTex2d, &HkProjectTex2d, &g_origProjectTex2d);
        // No-selection quest trigger: RenderAutoTrackCursor (the tracked-quest objective
        // cursor) fires per frame while a quest is selected — its invocation re-arms the
        // quest sweep with no unit targeted.
        const bool autoOk =
            wxl::core::hook::Install("selcircle-poc::autotrack", kRenderAutoTrackCursor, &HkRenderAutoTrack, &g_origRenderAutoTrack);
        // Unit GUID cache: POST hook on GetObjectByGuid — the proven kill-highlight mechanism
        // for learning which units the client has resolved (the ObjectManager hash walk was
        // DISPROVEN for this client; see the cache comment above).
        const bool objOk =
            wxl::core::hook::Install("selcircle-poc::getobject", off::kGetObjectByGuid, &HkGetObjectByGuid, &g_origGetObject);
        if (projOk)  wxl::core::hook::Enable(kProjectTex2d);
        if (autoOk)  wxl::core::hook::Enable(kRenderAutoTrackCursor);
        if (objOk)   wxl::core::hook::Enable(off::kGetObjectByGuid);

        // Per-frame visible-objects enumeration (the engine's own visible list): cdecl
        // int(cb, param), callback int(cb)(guidLo, guidHi, param) -> 0 stops.
        g_origEnumVisible = wxl::game::Native<EnumVisibleObjectsFn>(kEnumVisibleObjects);
        // Exact native unit selection-circle renderer used by the per-unit quest path.
        g_origRenderTargetSelection =
            wxl::game::Native<RenderTargetSelectionFn>(kRenderTargetSelection);
        // No-selection sweep trigger (HkRenderAutoTrack): camera-facing rotation matrix
        // builder for the synthesized recipe args. Two-argument __stdcall helper;
        // matrix/position are on the stack and the callee returns with `ret 8`.
        g_origBuildSelectionRotMatrix = wxl::game::Native<BuildSelectionRotMatrixFn>(kBuildSelectionRotMatrix);
        // The native circle recipe's texture bind + state push/pop (used by the autotrack
        // no-selection trigger — see the kRsPush/kSetCircleRenderStates/kRsPop comment).
        g_origRsPush = wxl::game::Native<RsPushFn>(kRsPush);
        g_origSetCircleRenderStates = wxl::game::Native<SetCircleRenderStatesFn>(kSetCircleRenderStates);
        g_origRsPop = wxl::game::Native<RsPopFn>(kRsPop);
        // Death check for the quest sweep: the client's own predicted-health getter.
        g_origGetPredictedHealth = wxl::game::Native<GetPredictedHealthFn>(kGetPredictedHealth);
        // Quest-ring custom texture: engine-native BLP load (CTexture::CreateFromFile ->
        // TextureGetGxTex mode=1 waits the async read and yields the GxTex wrapper state
        // 0x15 accepts).
        g_origCreateFromFile = wxl::game::Native<CreateFromFileFn>(kCreateFromFile);
        g_origTextureGetGxTex = wxl::game::Native<TextureGetGxTexFn>(kTextureGetGxTex);
        // Quest-ring texture state-0x15 swap.
        g_origRsSetPointer = wxl::game::Native<RsSetPointerFn>(kRsSetPointer);

        // Reset the quest-texture swap guard.
        g_questTexSwapDone = false;
        g_savedNativeGxTex = nullptr;

        // Once-per-frame gate reset; fires on Present.
        wxl::events::Subscribe(wxl::events::Event::OnFrame, &OnFrame, nullptr);
        // The visible-objects cache refresh runs at the world -> UI boundary (once per
        // frame); the hook's per-unit sweep (next frame's first circle draw — selection or
        // autotrack) consumes that cache, so quest mobs' circles render even with no unit
        // selected.
        wxl::events::Subscribe(wxl::events::Event::OnWorldRenderEnd, &OnWorldRenderEnd, nullptr);
        // Quest-ring texture load (once at first world entry + re-create on device reset).
        wxl::events::Subscribe(wxl::events::Event::OnWorldEnter, &OnWorldEnter, nullptr);
        wxl::events::Subscribe(wxl::events::Event::OnDeviceReset, &OnDeviceReset, nullptr);
        // Drop stale unit GUIDs when the world is torn down.
        wxl::events::Subscribe(wxl::events::Event::OnWorldLeave, &OnWorldLeave, nullptr);

        // Quest highlight: Lua bridge + the opcode-200 quest tracker script (port of the
        // kill-highlight mechanism with distinct names so both modules can coexist).
        wxl::runtime::lua::RegisterFunction("SetSelCircleQuestEntries", &ScriptSetQuestEntries);
        wxl::runtime::lua::RegisterFunction("ClearSelCircleQuestEntries", &ScriptClearQuestEntries);
        wxl::runtime::lua::RegisterScript("wxl-selcircle-poc-quest", R"lua(
            -- Custom-packet infrastructure (guarded: quest-marker may define it first).
            if not __callbacks then __callbacks = {} end
            if not __ReadCustomPacket then
                function __ReadCustomPacket()
                    local reader = {}
                    reader.ReadUInt8   = function() return _CLIENT_NETWORK(13) end
                    reader.ReadInt8    = function() return _CLIENT_NETWORK(14) end
                    reader.ReadUInt16  = function() return _CLIENT_NETWORK(15) end
                    reader.ReadInt16   = function() return _CLIENT_NETWORK(16) end
                    reader.ReadUInt32  = function() return _CLIENT_NETWORK(17) end
                    reader.ReadInt32   = function() return _CLIENT_NETWORK(18) end
                    reader.ReadUInt64  = function() return _CLIENT_NETWORK(17) end
                    reader.ReadInt64   = function() return _CLIENT_NETWORK(18) end
                    reader.ReadFloat   = function() return _CLIENT_NETWORK(21) end
                    reader.ReadDouble  = function() return _CLIENT_NETWORK(22) end
                    reader.ReadString  = function() return _CLIENT_NETWORK(23) end
                    reader.Size        = function() return _CLIENT_NETWORK(1) end
                    return reader
                end
            end
            if not OnCustomPacket then
                function OnCustomPacket(opcode, cb)
                    if __callbacks[opcode] == nil then __callbacks[opcode] = {} end
                    table.insert(__callbacks[opcode], cb)
                end
            end
            if not __FireCustomPacket then
                __FireCustomPacket = function(opcode)
                    if __callbacks[opcode] == nil then return end
                    for _, v in pairs(__callbacks[opcode]) do
                        v(__ReadCustomPacket())
                        _CLIENT_NETWORK(26)
                    end
                end
            end
            if not CreateCustomPacket then
                function CreateCustomPacket(opcode, size)
                    local writer = { id = _CLIENT_NETWORK(24, opcode, size) }
                    writer.WriteUInt8   = function(self, v) _CLIENT_NETWORK(2, self.id, v);  return self end
                    writer.WriteInt8    = function(self, v) _CLIENT_NETWORK(3, self.id, v);  return self end
                    writer.WriteUInt16  = function(self, v) _CLIENT_NETWORK(4, self.id, v);  return self end
                    writer.WriteInt16   = function(self, v) _CLIENT_NETWORK(5, self.id, v);  return self end
                    writer.WriteUInt32  = function(self, v) _CLIENT_NETWORK(6, self.id, v);  return self end
                    writer.WriteInt32   = function(self, v) _CLIENT_NETWORK(7, self.id, v);  return self end
                    writer.WriteUInt64  = function(self, v) _CLIENT_NETWORK(8, self.id, v);  return self end
                    writer.WriteInt64   = function(self, v) _CLIENT_NETWORK(9, self.id, v);  return self end
                    writer.WriteFloat   = function(self, v) _CLIENT_NETWORK(10, self.id, v); return self end
                    writer.WriteDouble  = function(self, v) _CLIENT_NETWORK(11, self.id, v); return self end
                    writer.WriteString  = function(self, v) _CLIENT_NETWORK(12, self.id, v); return self end
                    writer.Size         = function(self) return _CLIENT_NETWORK(0, self.id) end
                    function writer:Send()
                        _CLIENT_NETWORK(25, self.id)
                        return self
                    end
                    return writer
                end
            end

            -- Quest-entry tracker (opcode 200): the server answers the quest-marker
            -- module's opcode-103 quest-selection request with an opcode-200 payload
            -- carrying up to 4 kill/interaction creature entry IDs. Apply whichever
            -- response arrives last; clear on zero or world leave.
            -- Lockout/pending state is GLOBAL (guarded) like kill-highlight's: the script
            -- re-runs on every new FrameScript state, but the lockout frame is created
            -- once and its OnUpdate closure must keep seeing the SAME lockout flag.
            local scpFrame = CreateFrame("Frame")
            local scpRegistered = false
            if scpPacketLockout == nil then scpPacketLockout = false end
            if scpPendingCount == nil then scpPendingCount = 0 end
            if scpPendingEntries == nil then scpPendingEntries = {0, 0, 0, 0} end
            if not scpLockoutFrame then
                scpLockoutFrame = CreateFrame("Frame")
                scpLockoutFrame:Hide()
                scpLockoutFrame:SetScript("OnUpdate", function(self, dt)
                    self.elapsed = (self.elapsed or 0) + dt
                    if self.elapsed >= 1 then
                        if scpPendingCount > 0 then
                            SetSelCircleQuestEntries(scpPendingCount,
                                scpPendingEntries[1], scpPendingEntries[2],
                                scpPendingEntries[3], scpPendingEntries[4])
                            scpPendingCount = 0
                            scpPendingEntries = {0,0,0,0}
                        end
                        scpPacketLockout = false
                        self:Hide()
                    end
                end)
            end

            local function SetupScpPacketHandler()
                if scpRegistered then return end
                scpRegistered = true
                local ok = pcall(function()
                    OnCustomPacket(200, function(reader)
                        if not UnitGUID("player") then return end
                        local count = reader:ReadUInt32()
                        if not count or count > 4 then
                            ClearSelCircleQuestEntries()
                            return
                        end
                        local e1 = count >= 1 and reader:ReadUInt32() or 0
                        local e2 = count >= 2 and reader:ReadUInt32() or 0
                        local e3 = count >= 3 and reader:ReadUInt32() or 0
                        local e4 = count >= 4 and reader:ReadUInt32() or 0
                        -- Packet lockout: defer during zone transitions instead of dropping.
                        if scpPacketLockout then
                            scpPendingCount = count
                            scpPendingEntries = {e1, e2, e3, e4}
                            return
                        end
                        if count == 0 then ClearSelCircleQuestEntries(); return end
                        SetSelCircleQuestEntries(count, e1, e2, e3, e4)
                    end)
                end)
            end

            scpFrame:RegisterEvent("PLAYER_ENTERING_WORLD")
            scpFrame:RegisterEvent("PLAYER_LEAVING_WORLD")
            scpFrame:SetScript("OnEvent", function(self, event)
                if event == "PLAYER_LEAVING_WORLD" then
                    ClearSelCircleQuestEntries(); return
                end
                if event == "PLAYER_ENTERING_WORLD" then
                    scpPacketLockout = true
                    scpLockoutFrame.elapsed = 0
                    scpLockoutFrame:Show()
                    scpFrame:UnregisterEvent("PLAYER_ENTERING_WORLD")
                    SetupScpPacketHandler(); return
                end
            end)
        )lua");
    }

    struct Registrar
    {
        Registrar()
        {
            wxl::runtime::modules::Register("wxl-selcircle-poc", &InstallModule);
        }
    } g_registrar;
    #endif

    void SetQuestHighlightEntries(const uint32_t* entries, uint32_t count)
    {
        uint32_t normalized[kMaxQuestEntries] = {};
        uint32_t normalizedCount = 0;
        for (uint32_t i = 0; entries && i < count && i < kMaxQuestEntries; ++i)
        {
            if (!entries[i]) continue;
            bool duplicate = false;
            for (uint32_t j = 0; j < normalizedCount; ++j)
                if (normalized[j] == entries[i]) { duplicate = true; break; }
            if (!duplicate) normalized[normalizedCount++] = entries[i];
        }
        for (uint32_t i = 0; i < kMaxQuestEntries; ++i)
            g_questEntries[i] = normalized[i];
        g_questEntryCount = normalizedCount;
    }

    void ClearQuestHighlightEntries()
    {
        g_questEntryCount = 0;
        for (uint32_t& entry : g_questEntries) entry = 0;
    }

    bool InstallQuestHighlight()
    {
        const bool projOk = g_api->HookAttach(
            "quest-highlight::projecttex2d", kProjectTex2d,
            reinterpret_cast<void*>(&HkProjectTex2d),
            reinterpret_cast<void**>(&g_origProjectTex2d), WXL_HOOK_DEFAULT_PRIORITY) != 0;
        const bool autoOk = g_api->HookAttach(
            "quest-highlight::autotrack", kRenderAutoTrackCursor,
            reinterpret_cast<void*>(&HkRenderAutoTrack),
            reinterpret_cast<void**>(&g_origRenderAutoTrack), WXL_HOOK_DEFAULT_PRIORITY) != 0;
        const bool objOk = g_api->HookAttach(
            "quest-highlight::getobject", off::kGetObjectByGuid,
            reinterpret_cast<void*>(&HkGetObjectByGuid),
            reinterpret_cast<void**>(&g_origGetObject), WXL_HOOK_DEFAULT_PRIORITY) != 0;

        g_origEnumVisible = wxl::game::Native<EnumVisibleObjectsFn>(kEnumVisibleObjects);
        g_origRenderTargetSelection =
            wxl::game::Native<RenderTargetSelectionFn>(kRenderTargetSelection);
        g_origBuildSelectionRotMatrix = wxl::game::Native<BuildSelectionRotMatrixFn>(kBuildSelectionRotMatrix);
        g_origRsPush = wxl::game::Native<RsPushFn>(kRsPush);
        g_origSetCircleRenderStates = wxl::game::Native<SetCircleRenderStatesFn>(kSetCircleRenderStates);
        g_origRsPop = wxl::game::Native<RsPopFn>(kRsPop);
        g_origGetPredictedHealth = wxl::game::Native<GetPredictedHealthFn>(kGetPredictedHealth);
        g_origCreateFromFile = wxl::game::Native<CreateFromFileFn>(kCreateFromFile);
        g_origTextureGetGxTex = wxl::game::Native<TextureGetGxTexFn>(kTextureGetGxTex);
        g_origRsSetPointer = wxl::game::Native<RsSetPointerFn>(kRsSetPointer);

        g_questTexSwapDone = false;
        g_savedNativeGxTex = nullptr;
        ClearQuestHighlightEntries();

        g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnFrame), &OnFrame, nullptr);
        g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnWorldRenderEnd), &OnWorldRenderEnd, nullptr);
        g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnWorldEnter), &OnWorldEnter, nullptr);
        g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnDeviceReset), &OnDeviceReset, nullptr);
        g_api->Subscribe(static_cast<uint32_t>(wxl::events::Event::OnWorldLeave), &OnWorldLeave, nullptr);

        if (!projOk || !autoOk || !objOk)
            WLOG_ERROR("quest objective highlight hooks failed (project=%d auto=%d object=%d)",
                       projOk ? 1 : 0, autoOk ? 1 : 0, objOk ? 1 : 0);
        else
            WLOG_INFO("native quest objective highlight installed");
        return projOk && autoOk && objOk;
    }
} // namespace wxl_quest_marker
