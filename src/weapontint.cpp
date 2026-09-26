// ============================================================
// weapontint.cpp: runtime HSB re-tint of the worn Cash weapon (Coloring Prism)
//
// v83 has NO item-dye system, so the recolor is done here: the weapon's WZ
// canvases are cloned, transformed in HSV, and swapped into the WZ tree for
// exactly as long as it takes the client to build the avatar's action layers.
// The layers keep their own refs to the clones, the tree is put back, and the
// avatar is left holding recolored sprites while every other consumer of that
// weapon img (inventory icon, tooltip, another player wearing the same sticker)
// still sees the original pixels.
//
//   PrepareActionLayer(pAvatar)          <- Detoured here
//     |  Swap()      Character/Weapon/<sticker>.img/<action>/<frame>/<part>
//     |                   := tinted clone            (originals remembered)
//     |  <original PrepareActionLayer builds the layers from the tree>
//     |  Restore()   put every original back
//     v
//
// WHY THIS SHAPE, and not the two obvious alternatives:
//   * Recoloring the WZ canvas IN PLACE would tint the item everywhere and for
//     everyone -- the canvases are shared, cached resources.
//   * Substituting inside get_unknown / Ztl_variant_t::GetUnknown would put a
//     string compare on the hottest resource path in the client, for every
//     canvas it ever resolves.
// The swap window is the narrowest place where "this avatar, this weapon" is
// known and the canvases have not been read yet.
//
// PROVEN PRIMITIVES:
//   IWzProperty::item[name] = <value>                     (writes into the WZ tree)
//   PcCreateObject<IWzCanvasPtr>(L"Canvas") + Create()
//   IWzCanvas::CopyEx(..., CA_OVERWRITE, ...)
//   IWzCanvas::Create(w,h,mag,fmt) / cx / cy
// The one primitive this file introduces is IWzRawCanvas::_LockAddress, used to
// walk the cloned pixels. It is guarded and has a Getpixel/DrawRectangle
// fallback, so if it ever fails on this build the feature degrades to slow
// rather than to broken.
//
// -------------------------------------------------------------------------
// WARNING: CLIENT-BUILD-SPECIFIC ADDRESSES
// Every 0x00XXXXXX below is tied to THIS v83 MapleStory.exe (image base
// 0x400000). Proof for each is written beside it.
//
// This file Detours FIVE addresses and must be their only owner (a second Detour on
// any of them, from anywhere, is the documented hazard -- whichever installs last wins):
//   0x00453AD1  CAvatar::PrepareActionLayer   body: equips, hair, skin, item glow
//   0x00453696  CAvatar face-layer builder    the Eyes tab
//   0x00933990  CUser::ShowSkillEffect        the Skills tab
//   0x0093BEB9  cash effect show              cash effect items (5010000..5019999)
//   0x0093C218  cash effect apply             cash effect items
// All five were confirmed unhooked elsewhere in this DLL when it was ported.
// -------------------------------------------------------------------------
//
// SCOPE: every player in the map, not just the local one. Remote avatars are
// identified by the character name at CUser+0x11AC (see that constant) and matched
// against a table the server pushes for the current map; the local player's own
// avatar takes the live slider values while the prism window is open. TintForAvatar()
// is the single place that decides which of the three cases applies.
// ============================================================

#include "pch.h"
#include "hook.h"
#include "debug.h"
#include "weapontint.h"

#include "wvs/avatar.h"
#include "wvs/packet.h"
#include "wvs/util.h"
#include "ztl/ztl.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cwctype>
#include <cwchar>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <memory>
#include <unordered_set>
#include <vector>

namespace {

// =====================================================
// ADDRESSES
// =====================================================
// CAvatar::PrepareActionLayer: CAvatar's vtable is stored by its constructor
// (CAvatar::Constructor @0x0044FE6C) at 0x0044FFB1:
//     mov dword ptr [esi], 0xAF14D8
// and 0x00AF14D8[5] = 0x00453AD1. Slot 5 is PrepareActionLayer: CAvatar::
// SetRidingVehicle (0x00456E35, already wrapped in wvs/avatar.h) ends with
//     push 0 / push 0x64 / push 6 / mov ecx,esi / call dword ptr [eax+0x14]
// i.e. a 3-arg call through vtable offset 0x14 = slot 5, matching the
// PrepareActionLayer(nActionSpeed, nWalkSpeed, bKeyDown) declaration.
constexpr uintptr_t kAddr_CAvatar_PrepareActionLayer = 0x00453AD1;
// CAvatar's FACE layer builder, called from SetEmotion (0x00451D82). THIS FILE MUST OWN
// THIS ADDRESS OUTRIGHT -- see BuildFaceLayer_Hook for why it is needed, and
// INTEGRATION.md 4.3a for what to do if your DLL already hooks it.
constexpr uintptr_t kAddr_CAvatar_BuildFaceLayer     = 0x00453696;

// CUserLocal* singleton: non-null only inside a field.
constexpr uintptr_t kAddr_CUserLocal_Instance = 0x00BEBF98;

// CAvatar sits at CUser+0x88, for EVERY CUser: CUser::CUser (0x0092DCFC) runs
// `lea edi,[esi+0x88]; call 0x0044FE6C` (the CAvatar ctor) at 0x0092DD14.
constexpr size_t kOff_AvatarInUser = 0x88;

// CUser+0x11AC is the character name (a ZXString<char>, so the field is the char*).
// This is how a remote avatar is identified, and it is the piece that kept the tint
// local-only.
//
// Found by observation, not by tracing. The neighbour CUser::CUser stores its only
// argument into, +0x11A8, is dwCharacterId (v95 CUser::m_dwCharacterId); it is not used
// here because the CUserRemote construction path passes 0 there (0x004B22A5
// `xor edi,edi; push edi`) and the server's tint table is keyed by name anyway.
// 0x00BEBF94, a global that looked like it might be a user pool, is the CSoundMan
// singleton, so there is no pool to walk from it. A one-shot scan of CUserLocal for pointers to short
// printable strings reported exactly one plausible member, +0x11AC, holding the
// live character name on two different clients simultaneously ("pink" and "Admin").
//
// Names are unique per world, so they key the tint table directly and no character
// id is needed anywhere in this feature.
constexpr size_t kOff_CharacterNameInUser = 0x11AC;

// CClientSocket singleton + SendPacket, as used by every other custom opcode here.
constexpr uintptr_t kAddr_ClientSocket_Instance = 0x00BE7914;
constexpr uintptr_t kAddr_ClientSocket_Send     = 0x0049637B;

// CAvatar::SetMoveAction, vtable slot 4 (0x00AF14D8[4]): the CAvatar-level
// implementation. Called directly, not through the vtable; see SehReloadActionLayer.
constexpr uintptr_t kAddr_CAvatar_SetMoveAction = 0x004520F1;

// CAvatar::ClearActionLayer(int nLayerSet), `ret 4`. Wipes the selected 0x5DC
// layer set: 0xA2 entries at +0x10, 0xA2 at +0x298, 0x2E at +0x520. The client
// calls it as ClearActionLayer(0) from PrepareActionLayer @0x00453AF7 and
// ClearActionLayer(1) from SetMoveAction @0x00452120.
constexpr uintptr_t kAddr_CAvatar_ClearActionLayer = 0x00453A29;

// m_nMoveAction, written by CAvatar::Init from its nMA argument and compared by
// SetMoveAction at 0x004520FF.
constexpr size_t kOff_MoveAction = 0x4E8;

// =====================================================
// PROTOCOL
// =====================================================
constexpr uint8_t kAction_RequestSnapshot = 0;
constexpr uint8_t kAction_Apply           = 1;
constexpr uint8_t kAction_Restore         = 2;
// Look (hair / eye) variants. SEPARATE actions rather than apply/restore with a dummy
// address, because those two run the server's resolve(), which demands a Cash equip at
// a given inventory position -- hair and eyes satisfy none of that. The handler's
// switch ends in a default no-op, so an older server ignores 3/4 instead of desyncing.
constexpr uint8_t kAction_ApplyLook       = 3;
constexpr uint8_t kAction_RestoreLook     = 4;
// Skills. Named by SKILL ID rather than by inventory address, because a skill has none; that
// is why these are their own actions and not another layer on Apply/Restore.
constexpr uint8_t kAction_ApplySkill      = 5;
constexpr uint8_t kAction_RestoreSkill    = 6;

constexpr uint8_t kResp_Snapshot    = 1;
constexpr uint8_t kResp_Result      = 2;
constexpr uint8_t kResp_RemoteTable = 3;   // everyone in the map, keyed by name

// =====================================================
// STATE
// =====================================================
// g_mtx guards the two fields WeaponTint_HandleSync writes; that runs on the
// RECEIVE thread while every reader is on the main thread.
constexpr int kAvatarEquipSlots = AVATAR_EQUIP_SLOTS;

// itemId -> the tint stored on that item. One entry per DYED item; anything absent
// is vanilla.
using TintMap = std::unordered_map<int, WeaponTint>;

std::mutex g_mtx;
TintMap    g_saved;                       // the local player's dyed items
// Skill part keys protected by AdoptOptimistic. A snapshot that omits one of a skill's
// parts (cap, older server, the apply of another part still in flight) must not wipe the
// colour the player just confirmed on another part.
std::unordered_set<int> g_protectKeys;
WeaponTintResult g_lastResult = kTintResult_None;
// Set by the receive thread when a snapshot actually CHANGES the tint, consumed by
// WeaponTint_Tick on the main thread. The repaint cannot be done where the packet
// lands: clearing the clone cache and rebuilding the avatar's layers are both
// main-thread-only, and the receive thread reaching into them is how you get a
// torn layer mid-frame.
bool g_syncDirty = false;

// OTHER PLAYERS. The server pushes one entry per player in the map who is wearing a
// tinted Cash weapon, keyed by character name (unique per world, and the only
// identity the client can read off an arbitrary CAvatar -- see kOff_CharacterNameInUser).
std::unordered_map<std::string, TintMap> g_remoteTints;
// Last CAvatar seen wearing each name, recorded by TintForAvatar as the renderer
// hands them over. Purely a repaint shortcut: there is no way to enumerate CUsers
// in this repo, so this is how a tint update finds the avatar to rebuild. Entries
// go stale when a player leaves, which is why every use re-reads the name off the
// pointer and discards it on a mismatch.
std::unordered_map<std::string, CAvatar*> g_avatarByName;
// Names whose tint changed and whose avatar still needs repainting, drained by the tick.
std::vector<std::string> g_remoteDirty;

// Main thread only. While the prism window is open its sliders override the stored
// tint for the ONE item being dyed; everything else the player wears keeps its own.
WeaponTint g_preview;
int        g_previewItemId = 0;
bool       g_previewActive = false;
int        g_forcedScope   = 0;           // >0 while a DLL-owned preview avatar builds
CAvatar*   g_forcedAvatar  = nullptr;
TintMap    g_forcedTints;
// The Coloring Prism's CAvatar continues to build its face layer after Init returns.
// It must retain its own live tint until that avatar is released, but no other avatar
// may ever match this pointer (in particular, not the player in the field).
CAvatar*   g_prismPreviewAvatar = nullptr;

// =====================================================
// HSV
// =====================================================
// Straight HSV round trip on 8-bit channels. `hue` rotates, `chroma` and
// `bright` scale saturation and value by (100 + delta)%. Fully transparent
// pixels are skipped so the sprite's cutout is untouched, and a fully
// desaturated pixel keeps hue 0 -- which is why a pure-grey weapon only
// responds to Tone once Chroma has been raised, exactly like the live game.
inline void RgbToHsv(int r, int g, int b, float& h, float& s, float& v) {
    const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const int d  = mx - mn;
    v = mx / 255.0f;
    s = mx == 0 ? 0.0f : static_cast<float>(d) / mx;
    if (d == 0) {
        h = 0.0f;
    } else if (mx == r) {
        h = 60.0f * fmodf((static_cast<float>(g - b) / d), 6.0f);
    } else if (mx == g) {
        h = 60.0f * ((static_cast<float>(b - r) / d) + 2.0f);
    } else {
        h = 60.0f * ((static_cast<float>(r - g) / d) + 4.0f);
    }
    if (h < 0.0f) h += 360.0f;
}

inline void HsvToRgb(float h, float s, float v, int& r, int& g, int& b) {
    const float c = v * s;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rf, gf, bf;
    if      (h <  60.0f) { rf = c; gf = x; bf = 0; }
    else if (h < 120.0f) { rf = x; gf = c; bf = 0; }
    else if (h < 180.0f) { rf = 0; gf = c; bf = x; }
    else if (h < 240.0f) { rf = 0; gf = x; bf = c; }
    else if (h < 300.0f) { rf = x; gf = 0; bf = c; }
    else                 { rf = c; gf = 0; bf = x; }
    auto to8 = [](float f) {
        int n = static_cast<int>(f * 255.0f + 0.5f);
        return n < 0 ? 0 : (n > 255 ? 255 : n);
    };
    r = to8(rf + m); g = to8(gf + m); b = to8(bf + m);
}

// One BGRA8888 pixel, transformed in place. Returns false if nothing changed
// (fully transparent), so the caller can skip the write.
inline bool TintPixel(uint32_t& argb, const WeaponTint& t) {
    const uint32_t a = (argb >> 24) & 0xFF;
    int r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    // TRANSPARENT IS NOT INVISIBLE. The game composites a skill effect ADDITIVELY, which adds
    // the source RGB whatever its alpha says, so the colour painted into a sprite's transparent
    // fringe still reaches the screen as light. Skipping those pixels left a dyed effect wearing
    // an untinted halo in the world while the preview, which alpha-composites, showed nothing
    // there and looked like a different colour entirely.
    //
    // Transparent AND black is still skipped: there is no colour to rotate, and it is most of a
    // large effect frame, so this is what keeps the walk affordable.
    if (a == 0 && r == 0 && g == 0 && b == 0) return false;
    float h, s, v;
    RgbToHsv(r, g, b, h, s, v);
    // Rotate or set, per the sign; see WeaponTint in weapontint.h.
    if (t.IsAbsoluteHue()) h  = static_cast<float>(t.HueDegrees());
    else                   h += static_cast<float>(t.hue);
    while (h >= 360.0f) h -= 360.0f;
    while (h <    0.0f) h += 360.0f;
    // TOWARDS THE BOUND, not scaled by it. `s *= (100 + chroma)/100` pinned to 1.0 well
    // before the slider ran out on an already-saturated pixel, and it is a mathematical
    // no-op on an achromatic one -- a grey cape did nothing at any Chroma at all.
    // Interpolating towards 1 on the positive half and towards 0 on the negative makes
    // every position of both sliders do something.
    //
    // NOTE what this does to VALUE at the extremes: v + (1-v)*bright lifts pure black
    // towards white, where the old multiply left it at 0. Dark outlines therefore lighten
    // as Brightness rises, and at +100 every pixel reaches 1.0 and the sprite goes white.
    // That is inherent to "towards the bound" and is the price of the negative half and the
    // mid-range being useful.
    const float chroma = static_cast<float>(t.chroma) / 100.0f;
    const float bright = static_cast<float>(t.bright) / 100.0f;
    s = chroma < 0.0f ? s * (1.0f + chroma) : s + (1.0f - s) * chroma;
    v = bright < 0.0f ? v * (1.0f + bright) : v + (1.0f - v) * bright;
    if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
    if (v < 0.0f) v = 0.0f; else if (v > 1.0f) v = 1.0f;
    HsvToRgb(h, s, v, r, g, b);
    argb = (a << 24) | (r << 16) | (g << 8) | b;
    return true;
}

// =====================================================
// CANVAS CLONE + RECOLOR
// =====================================================
// SWAP COST INSTRUMENTATION
// =====================================================
// What one layer build's swap actually costs, so the walk's breadth is judged on
// measurement rather than on assumption.
//
// THE EXPENSIVE HALF IS CLONING, NOT SWAPPING. A swap is a property write; a clone is a
// per-pixel HSV pass plus one COM DrawRectangle per horizontal run of equal colour, and it
// happens once per (canvas, tint) and is cached thereafter. So a build that creates no new
// clones is nearly free however wide the walk, and only the builds that DO clone say
// anything -- in practice the first build after a tint changes, and each step of a drag.
//
// Offline the run counts came out at a median of 7,867 per whole equip img, p90 26,842, and
// a worst case of 162,434. What that is in milliseconds turns entirely on the cost of one
// COM call, which cannot be settled from the WZ data. Hence this.
struct SwapCost {
    int swaps  = 0;      // canvases substituted into the tree
    int clones = 0;      // of those, the ones that had to be built rather than cached
    int pixels = 0;      // pixels those clones wrote. NOT the COM call count: a run covers
                         // several pixels, so calls were always fewer than this.
    int bulk   = 0;      // clones written with ONE locked memcpy
    int perRun = 0;      // clones that fell back to one COM call per run
    long long cloneTicks = 0;   // QPC ticks spent INSIDE CloneTinted
};
SwapCost g_cost;
// One QPC pair per clone, not per pixel, so the instrument costs a few dozen calls per build
// and cannot meaningfully disturb what it is measuring. Everything the build spends that is
// NOT in here is the walk: enumeration, property lookups, QueryInterface and the writes.
struct CloneTimer {
    LARGE_INTEGER t0{};
    CloneTimer() { QueryPerformanceCounter(&t0); }
    ~CloneTimer() {
        LARGE_INTEGER t1;
        QueryPerformanceCounter(&t1);
        g_cost.cloneTicks += (t1.QuadPart - t0.QuadPart);
    }
};

// =====================================================
// Everything from here to CloneTinted deals in RAW COM pointers and PODs on
// purpose: MSVC refuses `__try` in any function that also has objects requiring
// unwinding (C2712), and _com_ptr_t / Ztl_variant_t both do. The smart-pointer
// half lives in CloneTinted, which uses C++ EH only.

// HOW THE PIXELS ARE READ AND WRITTEN -- and why it is this exact pair.
//
// READ: lock the clone's raw buffer once and copy it out wholesale. One COM call
// for the whole sprite instead of one per pixel.
//
// WRITE: `DrawRectangle`, batched into horizontal runs of equal colour.
//
// The asymmetry is not an oversight. Writing straight into the locked buffer was
// tried and it DOES change what the canvas reports -- a changed pixel read back
// through get_pixel after unlocking returns the new value -- but the weapon still
// renders in its original colours. So the locked buffer is not what the renderer
// consumes; there is a converted/uploaded copy behind it that a raw poke does not
// invalidate, and neither _UnlockAddress(rect) nor _UnlockAddress(NULL) publishes
// it. DrawRectangle goes through the canvas's own drawing path and does.
//
// Do not "optimise" this back into a locked write. It looks like it works, it
// passes a read-back check, and the weapon stays the colour it started.
//
// The lock is still used for READING, where it is only ever asked to hand back
// bytes it already holds -- which is the part that was verified to be coherent.

// Is rawCanvas[][] indexed by TILE or by PIXEL?
//
// It matters and it cannot be assumed: under pixel indexing every (tx, ty) in a tile loop lands
// back in the FIRST tile, so a clear reports success on every tile while only ever erasing the
// top-left one. That is precisely how this presented -- six tiles reported ok and the pixels
// past x = 256 survived.
//
// Answered by comparing the objects handed back for two indices, because that is READ ONLY.
// The tempting probe -- ask for an out-of-range index and see if it fails -- risks being handed
// a surface that is not there, and writing into one of those is how this area corrupted the
// heap before. The probe index is valid under both conventions by construction.
bool RawCanvasIndexIsTile(IWzCanvas* pCanvas, int tilesX, int tilesY) {
    int px = 0, py = 0;
    if (tilesX > 1)      px = 1;
    else if (tilesY > 1) py = 1;
    else return true;                          // one tile: the conventions agree

    IWzRawCanvas* a = nullptr;
    IWzRawCanvas* b = nullptr;
    __try {
        if (FAILED(pCanvas->get_rawCanvas(0, 0, &a))) a = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        a = nullptr;
    }
    __try {
        if (FAILED(pCanvas->get_rawCanvas(px, py, &b))) b = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        b = nullptr;
    }
    // Different objects means the index picked a different tile, so it is a tile index.
    const bool distinct = (a && b && a != b);
    if (a) { __try { a->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    if (b) { __try { b->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    return distinct;
}

// Copy ONE TILE's rectangle between the canvas and a caller buffer `bufW` pixels wide.
//
// (x0, y0) is where the tile starts in canvas pixels, `cols` x `rows` how much of it is real:
// the last column and band of tiles are SHORT, and copying a full tile there would run off the
// end of the allocation -- the same mistake that corrupted the heap when clone writes ignored
// tiling. Rows advance by THIS tile's own pitch, never by the canvas width.
//
// `bpp` 2 is A4R4G4B4 and is only ever read; every canvas written here is one we Create()d as
// A8R8G8B8. SEH leaf: raw COM pointers and PODs only (C2712).
bool CopyTileLocked(IWzCanvas* pCanvas, int ix, int iy, int x0, int y0, int cols, int rows,
                    uint32_t* buf, int bufW, int bpp, bool write) {
    if (write && bpp != 4) return false;
    IWzRawCanvas* raw = nullptr;
    __try {
        if (FAILED(pCanvas->get_rawCanvas(ix, iy, &raw))) raw = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        raw = nullptr;
    }
    if (!raw) return false;

    int pitch = 0;
    uint8_t* base = nullptr;
    __try {
        VARIANT vAddr;
        VariantInit(&vAddr);
        if (SUCCEEDED(raw->raw__LockAddress(&pitch, &vAddr))) {
            // Canvas.dll returns VT_BYREF|VT_UI4 (0x4013 observed): the pixel array
            // itself. `lVal` and `byref` are the same union slot on win32, and the
            // client's own consumer at 0x005DAA43 reads that slot straight as the
            // buffer, then advances rows by `pitch` (0x005DAA7B).
            const int base_vt = V_VT(&vAddr) & VT_TYPEMASK;
            if (base_vt == VT_I4 || base_vt == VT_UI4
                || base_vt == VT_INT || base_vt == VT_UINT) {
                base = (V_VT(&vAddr) & VT_BYREF)
                     ? reinterpret_cast<uint8_t*>(V_BYREF(&vAddr))
                     : reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(V_I4(&vAddr)));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        base = nullptr;
    }

    bool ok = false;
    if (base && pitch >= cols * bpp) {
        __try {
            for (int y = 0; y < rows; ++y) {
                uint8_t* row = base + static_cast<size_t>(y) * pitch;
                uint32_t* mem = buf + static_cast<size_t>(y0 + y) * bufW + x0;
                if (write) {
                    memcpy(row, mem, static_cast<size_t>(cols) * 4);
                } else if (bpp == 4) {
                    memcpy(mem, row, static_cast<size_t>(cols) * 4);
                } else {
                    const uint16_t* px = reinterpret_cast<const uint16_t*>(row);
                    for (int x = 0; x < cols; ++x) {
                        const uint16_t s = px[x];
                        mem[x] = ((((s >> 12) & 0xFu) * 17u) << 24)
                               | ((((s >>  8) & 0xFu) * 17u) << 16)
                               | ((((s >>  4) & 0xFu) * 17u) <<  8)
                               |  (((s        & 0xFu) * 17u));
                    }
                }
            }
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (base) {
        // A READ unlocks with NULL, exactly as the client's own reader does at 0x005DAA9C.
        // A WRITE names the rect it changed (IWzRawCanvas::_UnlockAddress takes `RECT* prc`),
        // so the renderer's converted copy of that tile is refreshed from the new pixels.
        RECT rcDirty = { 0, 0, cols, rows };
        __try { raw->raw__UnlockAddress(write ? &rcDirty : nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try { raw->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}

// The canvas's own size and tile size. SEH leaf; zeros on any failure.
void SehCanvasGeometry(IWzCanvas* pCanvas, unsigned int& cw, unsigned int& ch,
                       unsigned int& tw, unsigned int& th) {
    cw = ch = tw = th = 0;
    __try {
        if (FAILED(pCanvas->get_width(&cw)))      cw = 0;
        if (FAILED(pCanvas->get_height(&ch)))     ch = 0;
        if (FAILED(pCanvas->get_tileWidth(&tw)))  tw = 0;
        if (FAILED(pCanvas->get_tileHeight(&th))) th = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cw = ch = tw = th = 0;
    }
}

// Bulk copy of a WHOLE canvas through the raw lock, TILE BY TILE.
//
// WHY PER TILE. A canvas larger than the device tile size (256 here) is stored as SEVERAL raw
// canvases, and get_rawCanvas(0, 0) hands back only the FIRST ONE. Both bulk paths used to
// refuse such a canvas outright and fall back to one COM call per pixel (get_pixel) or per run
// (DrawRectangle) -- measured at ~350 ms for a single 278x96 Brandish frame and over a second
// for its 352x348 ones, which stalled the Skills-tab preview on every frame and the first
// in-game cast after a dye. Every skill effect frame past 256px is tiled; no equip sprite is.
//
// So each tile is fetched by its own index and locked on its own, the same walk
// WeaponTint_ClearCanvas has been running on the window canvas: RawCanvasIndexIsTile settles
// whether get_rawCanvas wants a TILE index or a PIXEL coordinate, and the last row/column of
// tiles is clipped to what is really there. An untiled canvas is the one-tile case of the same
// loop, so the old single-lock behaviour is unchanged for it.
//
// Every guard the two single-lock paths had is kept: the canvas must really be w x h (a Create
// that came back smaller took h rows of memcpy off the end of its allocation once), each tile's
// pitch must cover its columns, and every raw access is under SEH. Any failure returns false
// and the caller falls back to the slow path, which rewrites every pixel anyway.
bool TiledCopy(IWzCanvas* pCanvas, int w, int h, uint32_t* buf, int bpp, bool write) {
    unsigned int cw = 0, ch = 0, tw = 0, th = 0;
    SehCanvasGeometry(pCanvas, cw, ch, tw, th);
    if (static_cast<int>(cw) != w || static_cast<int>(ch) != h) {
        if (write) {
            LOG_ONCE("weapontint: bulk write refused, canvas is %ux%u but %dx%d was expected",
                     cw, ch, w, h);
        } else {
            LOG_ONCE("weapontint: locked read refused, canvas is %ux%u but %dx%d was expected",
                     cw, ch, w, h);
        }
        return false;
    }
    if (!tw || static_cast<int>(tw) > w) tw = static_cast<unsigned int>(w);   // untiled
    if (!th || static_cast<int>(th) > h) th = static_cast<unsigned int>(h);
    const int tW = static_cast<int>(tw), tH = static_cast<int>(th);
    const int tilesX = (w + tW - 1) / tW;
    const int tilesY = (h + tH - 1) / tH;
    const bool tiled = tilesX > 1 || tilesY > 1;
    const bool byTile = RawCanvasIndexIsTile(pCanvas, tilesX, tilesY);

    bool all = true;
    for (int ty = 0; ty < tilesY && all; ++ty) {
        for (int tx = 0; tx < tilesX && all; ++tx) {
            const int x0 = tx * tW, y0 = ty * tH;
            const int cols = (w - x0 < tW) ? (w - x0) : tW;
            const int rows = (h - y0 < tH) ? (h - y0) : tH;
            if (cols <= 0 || rows <= 0) continue;
            const int ix = byTile ? tx : x0;
            const int iy = byTile ? ty : y0;
            if (!CopyTileLocked(pCanvas, ix, iy, x0, y0, cols, rows, buf, w, bpp, write)) {
                all = false;
            }
        }
    }
    if (tiled) {
        // Once per direction: which path the big frames actually take.
        if (write) {
            LOG_ONCE("weapontint: tiled bulk write %dx%d over %dx%d tiles of %dx%d, indexed "
                     "by %s: %s", w, h, tilesX, tilesY, tW, tH, byTile ? "tile" : "pixel",
                     all ? "ok" : "FAILED, per-run fallback");
        } else {
            LOG_ONCE("weapontint: tiled locked read %dx%d over %dx%d tiles of %dx%d, indexed "
                     "by %s: %s", w, h, tilesX, tilesY, tW, tH, byTile ? "tile" : "pixel",
                     all ? "ok" : "FAILED, get_pixel fallback");
        }
    }
    return all;
}

// Read the whole canvas into `out` as ARGB8888. Returns false if the lock is
// unusable, which sends the caller to the per-pixel reader.
bool ReadPixelsLocked(IWzCanvas* pCanvas, int w, int h, uint32_t* out) {
    CANVAS_PIXFORMAT fmt = CP_UNKNOWN;
    __try {
        if (FAILED(pCanvas->get_pixelFormat(&fmt))) fmt = CP_UNKNOWN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fmt = CP_UNKNOWN;
    }
    const int bpp = (fmt == CP_A8R8G8B8) ? 4 : ((fmt == CP_A4R4G4B4) ? 2 : 0);
    if (bpp == 0) return false;
    return TiledCopy(pCanvas, w, h, out, bpp, false);
}

// Per-pixel reader, for a build where the lock is unusable.
void ReadPixelsSlow(IWzCanvas* pCanvas, int w, int h, uint32_t* out) {
    __try {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                uint32_t px = 0;
                if (FAILED(pCanvas->get_pixel(x, y, &px))) px = 0;
                out[static_cast<size_t>(y) * w + x] = px;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Write the tinted pixels back as horizontal runs of equal colour. Fully
// transparent pixels are skipped entirely -- they are already correct from the
// CopyEx, and they are the majority of a weapon sprite -- so a typical row costs a
// handful of calls rather than one per pixel. Returns the number of pixels changed.
// writeAll defines EVERY pixel, transparent ones included, instead of leaving them to whatever
// is already in the canvas. Only a mirrored clone needs that: see the note in CloneTinted.
int WriteRuns(IWzCanvas* pCanvas, int w, int h, const uint32_t* px, bool writeAll = false) {
    int changed = 0;
    __try {
        for (int y = 0; y < h; ++y) {
            const uint32_t* row = px + static_cast<size_t>(y) * w;
            int x = 0;
            while (x < w) {
                const uint32_t c = row[x];
                if (!writeAll && (c >> 24) == 0) { ++x; continue; }   // transparent: leave it
                int end = x + 1;
                while (end < w && row[end] == c) ++end;
                pCanvas->raw_DrawRectangle(x, y, end - x, 1, c);
                changed += end - x;
                x = end;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return changed;
}

// Every child name of a property node, read out in full before the caller touches
// the node. (Declared here because CloneTinted needs it; the swap walk below uses
// the same helper for the same collect-then-mutate reason, documented there.)
std::vector<std::wstring> ChildNames(IWzPropertyPtr pNode);

// Give the clone the SOURCE NODE'S WZ children -- origin, z, and above all `map`,
// the attachment points (hand / navel) the avatar assembler positions the weapon by.
//
// THIS IS NOT OPTIONAL. A canvas from PcCreateObject(L"Canvas") has an empty
// property collection, and the client's frame finalizer (0x004123D2, reached through
// 0x00407A14, whose caller is CActionMan::LoadCharacterAction) dereferences canvas->property
// WITHOUT a null check. Installing a property-less clone in the WZ tree is an
// access violation reading 0x00000000 the instant the avatar rebuilds -- which is
// exactly what a tinted preview did before this existed.
//
// Sub-nodes (`map` is itself a property) are shared by reference rather than deep
// copied: the client only ever reads them.
bool CopyCanvasProperty(IWzCanvasPtr src, IWzCanvasPtr dst) {
    IWzPropertyPtr sp, dp;
    try {
        sp = src->property;
        dp = dst->property;
    } catch (...) {
        return false;
    }
    if (!sp || !dp) return false;
    try {
        for (const std::wstring& name : ChildNames(sp)) {
            dp->item[name.c_str()] = sp->item[name.c_str()];
        }
    } catch (...) {
        return false;
    }
    return true;
}


// Bulk pixel write, the mirror of ReadPixelsLocked. Returns false if the lock is unusable,
// which leaves the caller to fall back to the per-run path.
//
// Only 32-bit. Every canvas this writes to is one we Create()d as CP_A8R8G8B8, so there is
// no packing case to get wrong.
//
// THE DESTINATION MUST REALLY BE w x h. Checking only `pitch >= w * 4` proves the rows are
// wide enough and says NOTHING about how many rows exist, so a canvas that came back smaller
// than asked for -- a Create that clamped, or a buffer not fully materialised because this
// canvas has deliberately never been CopyEx-ed -- took h rows of memcpy off the end of its
// allocation and corrupted the heap. The symptom was not a fault here: it was a crash later,
// in whatever freed next (an access violation inside operator delete tidying the pixel vector).
//
// TILING IS THE OTHER ONE THAT MATTERED. get_rawCanvas(0, 0) hands back only the FIRST TILE of
// a canvas past the device tile size, and writing w*h pixels into it linearly ran off the end
// of that tile and through whatever the heap put next -- which is why this only ever hurt on
// skill effect frames (median 16,560 pixels, up to 799,040) and never on equip sprites (median
// 208, single tile). That used to be a refusal; TiledCopy now writes such a canvas one tile at
// a time, each against its own pitch and clipped to its own size, and keeps both checks.
bool WritePixelsLocked(IWzCanvas* pCanvas, int w, int h, const uint32_t* in) {
    CANVAS_PIXFORMAT fmt = CP_UNKNOWN;
    __try {
        if (FAILED(pCanvas->get_pixelFormat(&fmt))) fmt = CP_UNKNOWN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fmt = CP_UNKNOWN;
    }
    if (fmt != CP_A8R8G8B8) return false;
    // The buffer is only READ on this direction; the cast is for the shared copier.
    return TiledCopy(pCanvas, w, h, const_cast<uint32_t*>(in), 4, true);
}

// ONE BULK WRITE INSTEAD OF ONE COM CALL PER RUN.
//
// Writing through the lock was tried once and rejected, and that note is worth restating
// because this is not a contradiction of it: a pixel poked into an ALREADY MATERIALISED
// canvas reads back changed through get_pixel while the renderer goes on showing the
// original, because the renderer consumes a converted copy that the poke never invalidates.
// That is still true.
//
// What changed is WHICH canvas is written. The old path was Create -> CopyEx -> poke, so the
// canvas had already been through CopyEx by the time of the poke. This one writes into a
// canvas that has never been CopyEx-ed and never been drawn, on the theory that its
// converted copy does not exist yet and will be built from what we wrote.
//
// IF THAT THEORY IS WRONG the symptom is specific and harmless: sprites render in their
// ORIGINAL colours while the log still reports clones being built. Set kBulkWrite to false
// and rebuild to go straight back to the per-run path, which is unchanged and still tested.
constexpr bool kBulkWrite = true;
IWzCanvasPtr CloneTinted(IWzCanvasPtr pSrc, const WeaponTint& t, bool mirror) {
    if (!pSrc) return nullptr;
    CloneTimer _timeThisClone;
    IWzCanvasPtr dst;
    try {
        const int w = static_cast<int>(pSrc->width);
        const int h = static_cast<int>(pSrc->height);
        // A sanity bound only. The real limit used to be the allocator, not the size: this
        // buffer is malloc'd now precisely so that large skill effect frames -- up to
        // 799,040 pixels against an equip's median 208 -- can be cloned like anything else.
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return nullptr;

        // A SCRATCH canvas, purely to normalise the source pixel format to 32-bit so the
        // pixel walk does not have to know about A4R4G4B4. The tinted result does NOT go
        // back into this one; see kBulkWrite.
        IWzCanvasPtr tmp;
        PcCreateObject<IWzCanvasPtr>(L"Canvas", tmp, nullptr);
        if (!tmp) return nullptr;
        tmp->Create(w, h, 0, CP_A8R8G8B8);
        // CA_OVERWRITE (255) copies the source verbatim, alpha included; the other
        // type, CA_REMOVEALPHA, would flatten the sprite cutout to opaque.
        tmp->CopyEx(0, 0, pSrc, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0);

        PcCreateObject<IWzCanvasPtr>(L"Canvas", dst, nullptr);
        if (!dst) return nullptr;
        dst->Create(w, h, 0, CP_A8R8G8B8);

        // Bail BEFORE tinting if the node cannot be made whole: a clone the client
        // would crash on must never reach the tree, and returning null here just
        // leaves that one sprite its original colour.
        if (!CopyCanvasProperty(pSrc, dst)) {
            LOG_ONCE("weapontint: canvas property could not be copied; that sprite "
                     "stays untinted rather than risking the frame finalizer");
            return nullptr;
        }

        // Read the normalised source once, tint in memory, then write the whole buffer.
        //
        // MALLOC, NOT std::vector, AND THAT IS LOAD-BEARING. This DLL routes operator
        // new/delete through the GAME's allocator (ZAllocEx), which is a four-bucket
        // small-block allocator -- 16/32/64/128 bytes -- with a raw path for anything larger
        // that re-derives the block size from a header word on free. Equip canvases are tiny
        // (median 208 pixels, so ~832 bytes) and have always survived that path. Skill effect
        // frames are two orders of magnitude bigger (median 16,560 pixels, up to 799,040) and
        // the free came apart on them: the crash was inside
        // vector<unsigned>::_Tidy -> operator delete -> ZAllocEx::s_Free, dereferencing null.
        //
        // malloc/free bypasses the override completely, so this buffer never touches the
        // game's allocator no matter how large it gets.
        const size_t nPixels = static_cast<size_t>(w) * static_cast<size_t>(h);
        struct RawPixels {
            uint32_t* p;
            explicit RawPixels(size_t n)
                : p(static_cast<uint32_t*>(::calloc(n, sizeof(uint32_t)))) {}
            ~RawPixels() { ::free(p); }
            RawPixels(const RawPixels&) = delete;
            RawPixels& operator=(const RawPixels&) = delete;
        } pxbuf(nPixels);
        if (!pxbuf.p) {
            LOG_ONCE("weapontint: could not allocate %ux%u pixels for a clone", w, h);
            return nullptr;
        }
        uint32_t* const px = pxbuf.p;
        const bool fastRead = ReadPixelsLocked(tmp, w, h, px);
        if (!fastRead) {
            LOG_ONCE("weapontint: raw lock unusable for reading; falling back to "
                     "get_pixel (%dx%d)", w, h);
            ReadPixelsSlow(tmp, w, h, px);
        }
        int tinted = 0;
        for (size_t i = 0; i < nPixels; ++i) {
            if (TintPixel(px[i], t)) ++tinted;
        }
        // MIRROR IN PLACE, while the pixels are already in a plain buffer. Effect art is
        // drawn for a character facing right, so a left-facing preview needs the sprite
        // reversed, not merely repositioned -- anything directional (a slash, a projectile)
        // points the wrong way otherwise.
        if (mirror) {
            for (int y = 0; y < h; ++y) {
                uint32_t* row = px + static_cast<size_t>(y) * w;
                for (int a = 0, b = w - 1; a < b; ++a, --b) {
                    const uint32_t tmpPx = row[a];
                    row[a] = row[b];
                    row[b] = tmpPx;
                }
            }
        }
        int written = 0;
        bool bulk = false;
        if (kBulkWrite) {
            bulk = WritePixelsLocked(dst, w, h, px);
            if (bulk) written = w * h;
        }
        if (!bulk) {
            // EVERY pixel, and no base underneath. Both halves of that matter:
            //
            //   The transparent ones carry colour that the world's additive compositing still
            //   draws, so leaving them to a pre-copy of the source left them UNTINTED.
            //
            //   And a pre-copy is the sprite facing the other way when this clone is mirrored,
            //   which showed through as two copies of the effect back to back.
            //
            // This used to be the ONLY path for tiled canvases, which every large skill effect
            // frame is; TiledCopy now writes those tile by tile, so this is the fallback for a
            // lock that fails. Both properties above still matter when it runs.
            written = WriteRuns(dst, w, h, px, true);
        }
        ++g_cost.clones;
        g_cost.pixels += written;
        if (bulk) ++g_cost.bulk; else ++g_cost.perRun;
        LOG_ONCE("weapontint: first clone %dx%d, fastRead=%d, bulkWrite=%d, %d px tinted, "
                 "%d px written", w, h, fastRead ? 1 : 0, bulk ? 1 : 0, tinted, written);
        // The origin is what positions the sprite on the avatar; losing it would
        // fling the weapon off the character.
        dst->cx = pSrc->cx;
        dst->cy = pSrc->cy;
    } catch (...) {
        return nullptr;
    }
    return dst;
}

// =====================================================
// CACHE
// =====================================================
// Keyed by ORIGINAL CANVAS **and** TINT.
//
// Keying on the canvas alone and dropping the map whenever the tint changed was correct
// while one tint was live at a time, but a single frame now applies SEVERAL: an outfit
// can carry a different colour per item, and each item's effect layers carry a second
// one of their own. Each lookup would invalidate the map the previous one had just
// filled, so every canvas would be re-cloned every frame. Pointers are 32-bit in this
// client, so the pair packs into a uint64 exactly.
inline uint64_t CloneKey(IWzCanvas* raw, const WeaponTint& t) {
    return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(raw)) << 32) | t.Key();
}
std::unordered_map<uint64_t, IWzCanvasPtr> g_clones;

// Mirrored clones live in their OWN map. The cache key packs a canvas pointer and a tint into
// exactly 64 bits with no room left for a flip bit, and widening it would touch the hottest
// lookup in the feature for the sake of one caller.
std::unordered_map<uint64_t, IWzCanvasPtr> g_mirrorClones;

// EVERY LIVE CLONE, BY RAW POINTER, so a walk can recognise its own work.
//
// A single pass can reach one canvas twice, because WZ nodes alias each other: an alias
// resolves to whatever is in the target slot, and if that slot was already swapped, what
// comes back is the CLONE. Tinting that produces a clone of a clone -- the colour applied
// twice -- and the walk then records the clone as the "original" to restore, so the alias
// slot keeps a permanently double-tinted canvas for the rest of the session. That is what
// left the head a different colour from the body after a relog.
//
// Kept in step with g_clones, never outliving it, so a pointer in here is always live and
// its address can never have been recycled by a different canvas.
std::unordered_set<IWzCanvas*> g_cloneSet;

// Did this module make that canvas? Then it is a slot the walk has already swapped.
bool IsOurClone(IWzCanvas* p) {
    return p && g_cloneSet.find(p) != g_cloneSet.end();
}

// The vanilla canvas a clone was made from. Skill `ball` nodes often UOL to `effect`
// frames that a body pass has already swapped; tinting the clone would double the colour
// and skipping the slot would leave the projectile in the main-effect colour. The original
// lives in the high half of the cache key.
IWzCanvas* OriginalCanvasOf(IWzCanvas* clone) {
    if (!clone) return nullptr;
    for (const auto& kv : g_clones) {
        if (kv.second && kv.second.GetInterfacePtr() == clone) {
            return reinterpret_cast<IWzCanvas*>(static_cast<uintptr_t>(kv.first >> 32));
        }
    }
    return nullptr;
}

// The iris path keeps its OWN clone map. Sharing g_clones would be a latent aliasing
// bug -- one key mapping to two different transforms if a canvas were ever reachable
// from both paths -- and the invalidation rule is identical, so separating costs nothing.
std::unordered_map<IWzCanvas*, IWzCanvasPtr> g_irisClones;
uint32_t g_irisClonesKey = 0;
bool     g_irisClonesValid = false;

void ClearClones() {
    g_clones.clear();
    g_mirrorClones.clear();
    g_cloneSet.clear();
    g_irisClones.clear();
    g_irisClonesValid = false;
}

// Drop only the clones made with ONE tint value, instead of the whole cache.
//
// A slider step changes ONE key's colour, but dropping everything made every OTHER dyed
// item the character wears re-clone as well. At roughly 0.68 COM DrawRectangle calls per
// opaque pixel -- the art is anti-aliased, so runs collapse to one or two pixels and there
// is no cheaper way to write them -- that is the difference between paying for one item per
// drag step and paying for the whole outfit.
//
// The tint value sits in the LOW 32 BITS of the cache key (see CloneKey), so the entries to
// evict are exactly the ones matching it. Two items dyed the identical colour will evict
// together, which costs one extra re-clone and is still correct.
void ClearClonesForTint(uint32_t tintKey) {
    for (auto it = g_mirrorClones.begin(); it != g_mirrorClones.end(); ) {
        if (static_cast<uint32_t>(it->first & 0xFFFFFFFFull) == tintKey) {
            if (it->second) g_cloneSet.erase(it->second.GetInterfacePtr());
            it = g_mirrorClones.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = g_clones.begin(); it != g_clones.end(); ) {
        if (static_cast<uint32_t>(it->first & 0xFFFFFFFFull) == tintKey) {
            if (it->second) g_cloneSet.erase(it->second.GetInterfacePtr());
            it = g_clones.erase(it);
        } else {
            ++it;
        }
    }
}

IWzCanvasPtr GetTinted(IWzCanvasPtr pSrc, const WeaponTint& t) {
    if (!pSrc) return nullptr;
    const uint64_t key = CloneKey(pSrc, t);
    auto it = g_clones.find(key);
    if (it != g_clones.end()) return it->second;
    IWzCanvasPtr clone = CloneTinted(pSrc, t, false);
    g_clones.emplace(key, clone);   // cache the failure too, so it is not retried per frame
    if (clone) g_cloneSet.insert(clone.GetInterfacePtr());
    return clone;
}


IWzCanvasPtr GetTintedMirrored(IWzCanvasPtr pSrc, const WeaponTint& t) {
    if (!pSrc) return nullptr;
    const uint64_t key = CloneKey(pSrc, t);
    auto it = g_mirrorClones.find(key);
    if (it != g_mirrorClones.end()) return it->second;
    IWzCanvasPtr clone = CloneTinted(pSrc, t, true);
    g_mirrorClones.emplace(key, clone);
    if (clone) g_cloneSet.insert(clone.GetInterfacePtr());
    return clone;
}

// =====================================================
// WZ-TREE SWAP
// =====================================================
struct Swap {
    IWzPropertyPtr parent;
    std::wstring   name;
    // WHAT THE SLOT HELD, which is not always a canvas: an alias slot holds the alias.
    // Restoring a resolved canvas over an alias is what silently destroyed the
    // indirection; see RawChildObject.
    IUnknownPtr    original;
};
std::vector<Swap> g_swaps;
// Where the two walkers record the originals they displace. Normally g_swaps, which
// SwapGuard empties when a layer build ends; the item-effect scope below points it at
// its own list, because that build finishes on a completely different schedule.
std::vector<Swap>* g_swapSink = &g_swaps;
struct SwapSinkTo {
    std::vector<Swap>* prev;
    explicit SwapSinkTo(std::vector<Swap>& v) : prev(g_swapSink) { g_swapSink = &v; }
    ~SwapSinkTo() { g_swapSink = prev; }
};
// Face construction is deferred beyond CAvatar::Init. These swaps belong only to
// the Coloring Prism's one preview avatar and are restored before that avatar is
// released or replaced; ordinary per-layer swaps continue to use g_swaps above.
std::vector<Swap> g_previewFaceSwaps;

// Action code -> the WZ node name under Character/Weapon/<id>.img. The client's
// own converter is get_action_name_from_code @0x004A8CE6; reusing it keeps this
// file from carrying a duplicate action table that could drift from the client's.
auto get_action_name_from_code =
    reinterpret_cast<Ztl_bstr_t*(__cdecl*)(Ztl_bstr_t*, int)>(0x004A8CE6);

// The prism dyes the weapon, not its glow: the modern dialog says so outright
// ("Special effects will not be dyed"), and effect canvases are additively
// blended, so hue-rotating them reads as a bug rather than a recolor.
// `effect`, `effect0`, `effect1`, ... and nothing else. `effectItemID` is a property,
// not art, and a prefix match would have counted it as glow.
bool IsEffectPart(const wchar_t* name) {
    if (!name || _wcsnicmp(name, L"effect", 6) != 0) return false;
    const wchar_t* rest = name + 6;
    if (!*rest) return true;
    while (*rest) {
        if (*rest < L'0' || *rest > L'9') return false;
        ++rest;
    }
    return true;
}

// Skill img UI canvases. A body walk of Skill/<job>.img/skill/<id> would otherwise
// recolour the skill-window icon along with hit/ball/mob.
bool IsSkillUiPart(const wchar_t* name) {
    return name && _wcsnicmp(name, L"icon", 4) == 0;
}

bool IsBallPart(const wchar_t* name) {
    return name && _wcsicmp(name, L"ball") == 0;
}

// Which dyeable PART a direct child of Skill/<img>/skill/<id> is, per the table in
// weapontint.h -- the same table the server validates actions 5/6 against, so it is matched
// CASE-SENSITIVELY and exactly, never by a looser rule. Anything that is not listed (icon*,
// level, req, action, info, common, summon, afterimage, weapon, masterLevel, ...) answers
// kSkillPart_None and is never swapped.
int SkillPartOf(const wchar_t* name) {
    if (!name || !*name) return kSkillPart_None;
    struct Named { const wchar_t* name; int part; };
    static const Named kExact[] = {
        { L"effect0", kSkillPart_Effect0 }, { L"effect1", kSkillPart_Effect1 },
        { L"effect2", kSkillPart_Effect2 }, { L"effect3", kSkillPart_Effect3 },
        { L"ball", kSkillPart_Ball }, { L"ball0", kSkillPart_Ball }, { L"ball1", kSkillPart_Ball },
        { L"hit", kSkillPart_Hit }, { L"hit0", kSkillPart_Hit }, { L"hit1", kSkillPart_Hit },
        { L"affected", kSkillPart_Affected }, { L"affected0", kSkillPart_Affected },
        { L"specialAffected", kSkillPart_Affected },
        { L"special", kSkillPart_Special },
        { L"prepare", kSkillPart_Prepare },
        { L"keydown", kSkillPart_Keydown }, { L"keydown0", kSkillPart_Keydown },
        { L"keydownend", kSkillPart_KeydownEnd },
        { L"repeat", kSkillPart_Repeat },
        { L"finish", kSkillPart_Finish },
        { L"screen", kSkillPart_Screen },
        { L"tile", kSkillPart_Tile },
        { L"mob", kSkillPart_Mob }, { L"mob0", kSkillPart_Mob },
    };
    for (const Named& n : kExact) {
        if (wcscmp(name, n.name) == 0) return n.part;
    }
    // `effect` itself and every other effect* name the table does not claim (effect4,
    // effect_ship, effectR, effecttemp) are the main effect.
    if (wcsncmp(name, L"effect", 6) == 0) return kSkillPart_Effect;
    return kSkillPart_None;
}

// What a child slot ACTUALLY holds, read before get_unknown resolves it away.
//
// THIS IS THE THING THAT MUST GO BACK ON RESTORE. A WZ alias (a UOL) resolves
// transparently, so a walk that reads through get_unknown sees the target canvas -- and if
// it restores THAT, it has replaced the indirection with a hard reference. The alias is
// then gone for the rest of the session, and worse, if the target had already been swapped
// this pass the alias resolves to OUR OWN CLONE, which the walk would tint a second time
// and then record as the "original". That is what left the head a different colour from the
// body after a relog.
//
// SKIPPING ALIASES IS NOT THE ANSWER, though it was the first attempt. A walk is scoped to
// the action being built, and a lot of art is reachable ONLY through an alias that points
// at a DIFFERENT action, which the walk therefore never visits:
//     prone/0/hair    -> proneStab/0/hair          (1988 of 17483 hair imgs)
//     prone/0/body    -> proneStab/0/body          (all 46 skin imgs)
//     heal/0/weapon   -> alert/1/weapon            (and swingO2, on two-handed weapons)
// Skipping those leaves the frame vanilla while the rest of the pose recolours, which is
// exactly how it gets reported: "prone goes back to normal", "some frames of my two-hander
// are not dyed".
//
// So: swap the alias slot like any other, and put the ALIAS back afterwards. The target is
// left untouched at its own address, and the clone guard below stops a second alias to the
// same target from tinting the clone again.
IUnknownPtr RawChildObject(Ztl_variant_t& v) {
    if (V_VT(&v) == VT_UNKNOWN) return IUnknownPtr(V_UNKNOWN(&v));
    if (V_VT(&v) == VT_DISPATCH) {
        return IUnknownPtr(reinterpret_cast<IUnknown*>(V_DISPATCH(&v)));
    }
    return nullptr;
}

// Every child name of a property node, read out in full before the caller touches
// the node.
//
// COLLECT FIRST, THEN MUTATE. Writing `pNode->item[name] = ...` while an
// IEnumVARIANT over that same node is still open invalidates the enumerator: the
// walk stops early or returns garbage, so most of the weapon's frames never get
// tinted and -- worse -- the half-mutated node is what CAvatar::Init then tries to
// build its layers from, which leaves the avatar with no sprites at all. Reading
// the names into a vector first costs one small allocation per frame and makes the
// mutation loop touch no live enumerator.
std::vector<std::wstring> ChildNames(IWzPropertyPtr pNode) {
    std::vector<std::wstring> names;
    if (!pNode) return names;
    IEnumVARIANTPtr pEnum = pNode->_NewEnum;
    if (!pEnum) return names;
    for (;;) {
        Ztl_variant_t v;
        ULONG got = 0;
        if (FAILED(pEnum->Next(1, &v, &got)) || got == 0) break;
        if (V_VT(&v) == VT_BSTR && V_BSTR(&v)) names.emplace_back(V_BSTR(&v));
    }
    return names;
}

// Recursively replace every canvas under `pNode` with its tinted clone, to any
// depth, remembering the originals in g_swaps.
//
// RECURSIVE ON PURPOSE. The categories do not agree on depth:
//     Accessory (face/eye/ear)   <emotion>/<canvas>              2 levels
//     Coat/Pants/Shoes/Glove/... <action>/<frame>/<part>         3
//     Cap/Cape                   same, plus a default/default    3
//     Cash weapon                <weaponType>/<action>/<frame>/<part>  4
// Walking for canvases instead of assuming a shape means one routine covers all of
// them, and anything not looked at yet.
// Which half of an item's art a walk is allowed to touch. The two are dyed separately --
// the reference dialog has a whole tab for effects -- and they must never be swapped in
// the same pass, because each carries its own tint.
enum class Layer { Body, Effects };

void SwapSubtree(IWzPropertyPtr pNode, const WeaponTint& t, int depth, Layer layer,
                 bool skipSkillUi = false, bool retintClones = false);

// Put a tinted clone in this one slot. `retintClones` is for skill split layers: a
// later pass (glow / ball) must still replace THIS alias even when get_unknown resolved
// to a canvas the body pass already swapped.
bool SwapCanvasSlot(IWzPropertyPtr pNode, const std::wstring& name,
                    IUnknownPtr pRaw, IWzCanvasPtr pOrig, const WeaponTint& t,
                    bool retintClones) {
    if (!pNode || !pOrig) return false;
    IWzCanvasPtr src = pOrig;
    if (IsOurClone(pOrig.GetInterfacePtr())) {
        if (!retintClones) return false;
        IWzCanvas* orig = OriginalCanvasOf(pOrig.GetInterfacePtr());
        if (!orig || orig == pOrig.GetInterfacePtr()) return false;
        src = orig;
    }
    IWzCanvasPtr pTinted = GetTinted(src, t);
    if (!pTinted) return false;
    if (pTinted.GetInterfacePtr() == pOrig.GetInterfacePtr()) return false;
    if (!pRaw) pRaw = static_cast<IUnknown*>(pOrig.GetInterfacePtr());
    g_swapSink->push_back(Swap{ pNode, name, pRaw });
    ++g_cost.swaps;
    pNode->item[name.c_str()] = static_cast<IUnknown*>(pTinted);
    return true;
}

// Effects live under a node whose NAME starts with "effect", so an effects walk descends
// only into those and a body walk skips them. `effectItemID` is a property, not art, and
// is excluded from both by the canvas check further down.
void SwapEffectSubtrees(IWzPropertyPtr pNode, const WeaponTint& t, int depth) {
    if (!pNode || depth > 4) return;
    for (const std::wstring& name : ChildNames(pNode)) {
        Ztl_variant_t v = pNode->item[name.c_str()];
        IUnknownPtr pRaw = RawChildObject(v);       // the alias itself, when it is one
        IUnknownPtr pUnk = get_unknown(v);
        if (!pUnk) continue;
        const bool isEffect = IsEffectPart(name.c_str());

        // AN EFFECT IS USUALLY THE CANVAS, NOT A FOLDER OF THEM. The common shape is
        // <action>/<frame>/effect, a leaf canvas sitting beside the `weapon` canvas it
        // plays over; a folder of frames under `effect` is the rare one. Measured over
        // every equip that has effect art: 922 leaf canvases against 1 folder.
        //
        // So the canvas test has to come FIRST, and it has to happen here rather than in
        // SwapSubtree. A canvas answers QueryInterface(IWzProperty) too, because it
        // carries origin / z / map as children, so descending on the property interface
        // walks INTO the effect and finds only vectors and ints. That silently tinted
        // nothing at all: no crash, no log line, just an untinted glow.
        if (isEffect) {
            IWzCanvasPtr pOrig;
            if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pOrig)) && pOrig) {
                SwapCanvasSlot(pNode, name, pRaw, pOrig, t, false);
                continue;
            }
        }

        IWzPropertyPtr pSub;
        if (FAILED(pUnk.QueryInterface(__uuidof(IWzProperty), &pSub)) || !pSub) continue;
        if (isEffect) SwapSubtree(pSub, t, depth + 1, Layer::Effects);
        else          SwapEffectSubtrees(pSub, t, depth + 1);
    }
}

// One dyeable part of a skill and the colour it takes for this cast.
struct SkillPartTint {
    int        part = kSkillPart_None;
    WeaponTint tint;
};

// Swap every DIRECT child of a skill node that is a dyeable part with a colour in `tints`,
// each with its own colour. Children that are not parts are never looked at, which is the
// whole reason the icon (and level / req / summon / afterimage) keep their pixels.
//
// Inside a part NOTHING is skipped: the part is effect art from its root down, so the walk
// runs with Effects semantics and no name filter. A part that is itself a canvas (a one-frame
// node) is swapped at its own slot.
//
// ALWAYS RETINT (see SwapCanvasSlot). Parts alias each other -- `ball/0` into `effect/0` is
// common, and 1311002's whole `effect` / `hit` are UOLs into 1311001's -- so a slot can resolve
// to a clone another part installed a moment earlier in this same pass. Retinting takes that
// clone's ORIGINAL and dyes it this part's colour; the alias itself is what gets recorded and
// put back, so the indirection survives, and a clone is never tinted a second time.
void SwapSkillParts(IWzPropertyPtr pSkill, const std::vector<SkillPartTint>& tints) {
    if (!pSkill || tints.empty()) return;
    for (const std::wstring& name : ChildNames(pSkill)) {
        const int part = SkillPartOf(name.c_str());
        if (part == kSkillPart_None) continue;
        const WeaponTint* t = nullptr;
        for (const SkillPartTint& pt : tints) {
            if (pt.part == part) { t = &pt.tint; break; }
        }
        if (!t) continue;
        Ztl_variant_t v = pSkill->item[name.c_str()];
        IUnknownPtr pRaw = RawChildObject(v);       // the alias itself, when it is one
        IUnknownPtr pUnk = get_unknown(v);
        if (!pUnk) continue;
        IWzCanvasPtr pOrig;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pOrig)) && pOrig) {
            SwapCanvasSlot(pSkill, name, pRaw, pOrig, *t, true);
            continue;
        }
        IWzPropertyPtr pSub;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzProperty), &pSub)) && pSub) {
            SwapSubtree(pSub, *t, 1, Layer::Effects, false, true);
        }
    }
}

void SwapSubtree(IWzPropertyPtr pNode, const WeaponTint& t, int depth, Layer layer,
                 bool skipSkillUi, bool retintClones) {
    if (!pNode || depth > 4) return;
    for (const std::wstring& name : ChildNames(pNode)) {
        // On a BODY walk these are skipped -- the real game says special effects are not
        // dyed with the item, and an additive glow reads as a bug when hue-rotated. On an
        // EFFECTS walk the caller has already descended into one, so everything below is
        // fair game.
        if (layer == Layer::Body && IsEffectPart(name.c_str())) continue;
        if (skipSkillUi && IsSkillUiPart(name.c_str())) continue;
        if (skipSkillUi && layer == Layer::Body && IsBallPart(name.c_str())) continue;

        Ztl_variant_t v = pNode->item[name.c_str()];
        IUnknownPtr pRaw = RawChildObject(v);   // the alias itself, when it is one
        IUnknownPtr pUnk = get_unknown(v);
        if (!pUnk) continue;                 // delay / z ints, origin vectors

        // Canvas first: a canvas node also carries child properties (origin, z,
        // map), so testing for IWzProperty first would recurse into it instead.
        IWzCanvasPtr pOrig;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pOrig)) && pOrig) {
            SwapCanvasSlot(pNode, name, pRaw, pOrig, t, retintClones);
            continue;
        }
        IWzPropertyPtr pSub;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzProperty), &pSub)) && pSub) {
            SwapSubtree(pSub, t, depth + 1, layer, skipSkillUi, retintClones);
        }
    }
}

// Item id -> the Character subdirectory its sprites live in.
//
// The client's own mapper is get_equip_data_path @0x005C94A1, but it returns a
// ZXString<wchar_t> by out-param and pulls the name out of the obfuscated string
// pool, so reproducing the (tiny, stable) table is simpler than marshalling that.
// Both agree on `id / 10000` as the key; the weapon arm is the one that widens, to
// `id / 100000 in {13,14,16,17}`.
const wchar_t* CharacterSubdirOf(int itemId) {
    if (itemId <= 0) return nullptr;
    const int hi = itemId / 100000;
    if (hi == 13 || hi == 14 || hi == 16 || hi == 17) return L"Weapon";
    switch (itemId / 10000) {
        case 100: return L"Cap";
        case 101: case 102: case 103: return L"Accessory";   // face / eye / earring
        case 104: return L"Coat";
        case 105: return L"Longcoat";
        case 106: return L"Pants";
        case 107: return L"Shoes";
        case 108: return L"Glove";
        case 109: return L"Shield";
        case 110: return L"Cape";
        case 111: return L"Ring";
        case 112: return L"Accessory";                        // pendant
        // Mounts and saddles. Without this arm the walk returned nullptr and did nothing,
        // while the drop gate, the server and the snapshot all accepted the dye happily --
        // so a player could spend a prism on a mount, have the colour stored and broadcast,
        // and never see it. 0x005C9864 maps 191 to the same pool string as 190.
        case 190: case 191: return L"TamingMob";
        default:  return nullptr;
    }
}

// The weapon-type code a Cash weapon's art is filed under.
//
// A Cash weapon nests its actions one level deeper, under the numeric code of the weapon
// type the sticker is being worn OVER -- 01702000.img/33/stand1/0/weapon, where 33 is the
// dagger code. The code is the BASE weapon's id folded down, `id / 10000 - 100`, checked
// against every numeric subdir in the tree: every code a base weapon in this client can
// produce appears there, and the leftovers (61..78) belong to weapon types this version
// has no items for at all.
//
// ONLY THE WIELDED CODE IS EVER DRAWN. 01701000.img carries 29 of them, so walking them all
// cloned 29 subtrees to put one on screen.
int WeaponTypeCodeOf(int baseWeaponItemId) {
    if (baseWeaponItemId <= 0) return -1;
    const int code = baseWeaponItemId / 10000 - 100;
    return (code > 0 && code < 100) ? code : -1;
}

// Dispatch a subtree to whichever walk the layer asks for.
void SwapLayerOf(IWzPropertyPtr p, const WeaponTint& t, Layer layer) {
    if (layer == Layer::Effects) SwapEffectSubtrees(p, t, 0);
    else                         SwapSubtree(p, t, 0, Layer::Body);
}


// =====================================================
// SWAP PLAN CACHE
// =====================================================
// Every canvas slot in one img, resolved once and kept.
//
// WHY THIS IS SOUND: a WZ img is static data. The tree of (parent, child name) pairs and
// which of them hold canvases is fixed the moment the img is loaded, so re-deriving it per
// build was pure repetition -- measured at 16.2ms of a 21ms build, against 4.9ms for all the
// pixel work put together.
//
// The plan holds REFERENCES to the parent nodes and the source canvases, which pins them for
// the life of the process. That is deliberate: it is also what makes the plan safe, since a
// node that cannot be freed cannot be dangling. The cost is bounded by the number of
// distinct dyed imgs a session touches.
//
// KEYED ON THE LIVE IMG POINTER, not on the path. If the resource manager ever drops and
// rebuilds a tree, GetObjectA hands back a different object, the lookup misses, and a fresh
// plan is built against the tree that is actually live. A stale plan simply stops being
// found. That is the whole invalidation rule.
//
// `original` is the RAW slot contents captured at rest -- the alias object where the slot
// holds one, the canvas otherwise -- for the same reason the walk records it: restoring a
// resolved canvas over an alias destroys the indirection. A plan is only ever built from a
// slot at rest, because an img's plan is built on the first call that touches that img, and
// nothing has swapped it yet at that point.
struct PlanEntry {
    IWzPropertyPtr parent;
    std::wstring   name;
    IUnknownPtr    original;    // what the slot held before anything touched it
    IWzCanvasPtr   canvas;      // the source canvas to tint
    Layer          layer;       // which pass this slot belongs to
};

struct SwapPlan {
    std::vector<PlanEntry> entries;
};

std::unordered_map<IWzProperty*, std::shared_ptr<SwapPlan>> g_plans;

// Record every canvas slot under `pNode`, tagging each with the pass it belongs to. The
// shape mirrors the old walk exactly, including the canvas-before-property test: a canvas
// answers QueryInterface(IWzProperty) too, so testing for a property first would descend
// INTO the canvas and find only its origin and z children.
void BuildPlanFrom(IWzPropertyPtr pNode, SwapPlan& plan, int depth, bool inEffect) {
    if (!pNode || depth > 6) return;
    for (const std::wstring& name : ChildNames(pNode)) {
        if (name == L"info") continue;
        Ztl_variant_t v = pNode->item[name.c_str()];
        IUnknownPtr pRaw = RawChildObject(v);
        IUnknownPtr pUnk = get_unknown(v);
        if (!pUnk) continue;                     // delay / z ints, origin vectors
        const bool isEffect = inEffect || IsEffectPart(name.c_str());

        IWzCanvasPtr pCanvas;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pCanvas)) && pCanvas) {
            if (!pRaw) pRaw = static_cast<IUnknown*>(pCanvas.GetInterfacePtr());
            plan.entries.push_back(PlanEntry{
                pNode, name, pRaw, pCanvas,
                isEffect ? Layer::Effects : Layer::Body });
            continue;
        }
        IWzPropertyPtr pSub;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzProperty), &pSub)) && pSub) {
            BuildPlanFrom(pSub, plan, depth + 1, isEffect);
        }
    }
}

// The plan for one img root, built on first use.
std::shared_ptr<SwapPlan> PlanFor(IWzPropertyPtr pImg) {
    if (!pImg) return nullptr;
    IWzProperty* key = pImg.GetInterfacePtr();
    auto it = g_plans.find(key);
    if (it != g_plans.end()) return it->second;
    auto plan = std::make_shared<SwapPlan>();
    BuildPlanFrom(pImg, *plan, 0, false);
    g_plans.emplace(key, plan);
    LogMessage("weapontint: swap plan built: %d slots", static_cast<int>(plan->entries.size()));
    return plan;
}

// Apply one pass of a plan. This is the whole per-build cost now: a tint lookup and a
// property write per slot, with no enumeration, no name lookup and no QueryInterface.
void ApplyPlan(const std::shared_ptr<SwapPlan>& plan, const WeaponTint& t, Layer layer) {
    if (!plan) return;
    for (const PlanEntry& e : plan->entries) {
        if (e.layer != layer) continue;
        IWzCanvasPtr pTinted = GetTinted(e.canvas, t);
        if (!pTinted) continue;
        g_swapSink->push_back(Swap{ e.parent, e.name, e.original });
        ++g_cost.swaps;
        try {
            e.parent->item[e.name.c_str()] = static_cast<IUnknown*>(pTinted);
        } catch (...) {
        }
    }
}
// Tint one worn item's sprites for the action currently being built.
//
// Scoped to the action on purpose. Walking a whole img would be a few hundred
// canvases per item, and a full outfit re-tinted at drag speed would stall the
// client; one action is a handful. Items with no body actions (accessories, which
// are keyed by face emotion) have so few canvases that all of them are walked.


void SwapInTintFor(int itemId, const wchar_t* /*actionName*/, const WeaponTint& t,
                   Layer layer, int weaponTypeCode) {
    const wchar_t* subdir = CharacterSubdirOf(itemId);
    if (!subdir) return;
    try {
        wchar_t path[80];
        _snwprintf_s(path, _countof(path), _TRUNCATE,
                     L"Character/%s/%08d.img", subdir, itemId);
        IWzPropertyPtr pImg = get_rm()->GetObjectA(path).GetUnknown();
        if (!pImg) return;

        // The weapon-type narrowing happens at PLAN level, so the plan for a Cash weapon
        // covers only the type actually wielded. A player who changes base weapon type gets
        // a different root here and therefore a different plan, which is correct.
        IWzPropertyPtr pRoot = pImg;
        if (weaponTypeCode >= 0) {
            wchar_t code[8];
            _snwprintf_s(code, _countof(code), _TRUNCATE, L"%d", weaponTypeCode);
            Ztl_variant_t v = pImg->item[code];
            IUnknownPtr pUnk = get_unknown(v);
            IWzPropertyPtr pType;
            if (pUnk && SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzProperty), &pType)) && pType) {
                pRoot = pType;
            }
        }
        ApplyPlan(PlanFor(pRoot), t, layer);
    } catch (...) {
        // Whatever landed in g_swaps is still undone by SwapOut.
    }
}

// =====================================================
// LOOK TINTS -- hair and eye colour (the window's Hair / Face tabs)
// =====================================================
// Hair is easy and eyes are not, and it is worth saying exactly why.
//
// HAIR. Character/Hair/000300NN.img holds ONE real canvas per layer and every
// per-action frame is a UOL pointing back at it: stand1/0/hair, walk1/0/hair,
// alert/0/hair, prone/0/hair and fly/0/hair all hold the string "../../default/hair".
// So three canvases cover the whole character -- `default/hair` plus `backDefault`'s
// two, `backHairBelowCap` and `backHair`. That last node is the trap: the equip walk
// visits { actionName, "default" } only, which reaches the front hair and not the
// back, and the result is a character whose ponytail is still the old colour.
//
// EYES. There is no iris layer to tint. A v83 Face img is one flat canvas per
// EXPRESSION containing eyes, eyebrows and mouth over transparency, and vanilla
// changes eye colour by swapping to a wholly different, separately drawn img. So a
// naive hue rotation of the canvas is wrong twice over:
//
//   * it recolours things that are not eyes. Measured over 117 default/face canvases:
//     a mean of 10 non-iris pixels per canvas are chromatic enough to move visibly
//     (brown brows/lashes at H~30, blue sclera shading at H~210-240), and smile /
//     angry / love carry 21-100 pure-red MOUTH pixels that a rotation turns green.
//   * it does nothing at all for the two most common eye colours. Colour 0 (Black)
//     is achromatic in 104 of 112 measured styles and colour 8 (White) in 102 of 109,
//     and TintPixel's hue rotation and chroma scale are both mathematical no-ops on
//     an S=0 pixel -- so a black-eyed player would see their whole face outline
//     brighten under the Value slider and their eyes never change.
//
// Thresholding the worn canvas cannot separate the iris either: those Black irises
// sit at S=0.000 exactly while 4-bit quantisation artefacts in the sclera reach
// S=1.000, so a saturation rule fails in both directions simultaneously.
//
// WHAT WORKS is to ask the artists. Eye colour is the HUNDREDS DIGIT of the face id,
// so the same style in another colour is one arithmetic step away, and DIFFING those
// siblings yields exactly the pixels an eye-colour change is allowed to touch -- for
// 00020000.img/default/face that is 10 pixels out of 416, the same 10 for every
// colour pair. Then the masked pixels are REPLACED from a chromatic reference and
// rotated from there, which is what makes the slider mean the same thing whatever
// colour the player happens to be wearing.
//
// Measured against a 7-variant ground truth: 1.5% of true iris pixels missed, 1.0%
// spurious (and those are achromatic, so hue and chroma are no-ops on them anyway),
// 91 of 117 canvases pixel-exact. On smile/angry the mask comes out empty, so those
// canvases are never cloned and the red mouth is never touched.
// The face family's hundreds digit is the native eye-colour variant. Keep the
// style suffix intact and compare three chromatic siblings within that family.
constexpr int kFaceRefOffsets[3] = { 200, 400, 600 };     // Red, Brown, Purple
constexpr int kIrisChannelDelta  = 34;    // 2 nibble steps in ARGB4444, the face format
constexpr float kIrisMinSat      = 0.20f;
constexpr float kIrisMinVal      = 0.15f;

// The only expression canvases that carry eye colour. Everything else (smile, angry,
// cry, love, every closed-eye pose) is byte-identical across all nine colour variants,
// so walking them would be pure cost -- 5 canvases instead of ~35.
const wchar_t* const kFaceIrisPaths[] = {
    L"default/face", L"blink/0/face", L"blink/2/face", L"wink/0/face", L"hit/0/face",
};

// A face style's iris mask plus the reference pixels the tint is generated FROM.
// Independent of the tint, so it is computed once per face id and survives every
// slider drag; the clone cache above it is what the tint invalidates.
struct FaceIrisPlan {
    int w = 0, h = 0;
    std::vector<uint8_t>  mask;   // 1 = iris
    std::vector<uint32_t> ref;    // the Red variant's pixels, ARGB
    bool Empty() const { return mask.empty(); }
};
std::unordered_map<int, std::vector<FaceIrisPlan>> g_facePlans;   // faceId -> per path

// Read any canvas into ARGB by cloning it first. Reading the ResMan-owned original
// through a lock would mean holding a lock on a process-global node; CopyEx into a
// scratch canvas is the path CloneTinted already proves out.
bool ReadCanvasPixels(IWzCanvasPtr pSrc, int& w, int& h, std::vector<uint32_t>& out) {
    if (!pSrc) return false;
    try {
        w = static_cast<int>(pSrc->width);
        h = static_cast<int>(pSrc->height);
        if (w <= 0 || h <= 0 || w > 2048 || h > 2048) return false;
        IWzCanvasPtr tmp;
        PcCreateObject<IWzCanvasPtr>(L"Canvas", tmp, nullptr);
        if (!tmp) return false;
        tmp->Create(w, h, 0, CP_A8R8G8B8);
        tmp->CopyEx(0, 0, pSrc, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0);
        out.assign(static_cast<size_t>(w) * h, 0);
        if (!ReadPixelsLocked(tmp, w, h, out.data())) ReadPixelsSlow(tmp, w, h, out.data());
    } catch (...) {
        return false;
    }
    return true;
}

IWzPropertyPtr FaceImg(int faceId) {
    try {
        wchar_t path[80];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"Character/Face/%08d.img", faceId);
        return IWzPropertyPtr(get_rm()->GetObjectA(path).GetUnknown());
    } catch (...) {
        return nullptr;
    }
}

// Walk a slash-separated node path. UOL nodes resolve natively on the way, which
// matters here: several imported face styles reach an expression through a uol
// rather than holding its canvas directly.
IWzPropertyPtr PropertyAtPath(IWzPropertyPtr pImg, const std::wstring& path) {
    if (!pImg) return nullptr;
    try {
        IWzPropertyPtr cur = pImg;
        size_t at = 0;
        while (at <= path.size()) {
            const size_t slash = path.find(L'/', at);
            const std::wstring seg =
                path.substr(at, slash == std::wstring::npos ? std::wstring::npos : slash - at);
            if (!seg.empty()) {
                IUnknownPtr pUnk = cur->item[seg.c_str()].GetUnknown();
                if (!pUnk) return nullptr;
                IWzPropertyPtr nxt;
                if (FAILED(pUnk.QueryInterface(__uuidof(IWzProperty), &nxt)) || !nxt)
                    return nullptr;
                cur = nxt;
            }
            if (slash == std::wstring::npos) break;
            at = slash + 1;
        }
        return cur;
    } catch (...) {
        return nullptr;
    }
}

// The canvas at a path, or null. Canvas is checked FIRST because a canvas also
// answers QueryInterface(IWzProperty) -- the same ordering trap SwapSubtree calls out.
IWzCanvasPtr CanvasAtPath(IWzPropertyPtr pImg, const wchar_t* path) {
    if (!pImg) return nullptr;
    try {
        const std::wstring full(path);
        const size_t cut = full.find_last_of(L'/');
        IWzPropertyPtr pParent = (cut == std::wstring::npos)
            ? pImg : PropertyAtPath(pImg, full.substr(0, cut));
        if (!pParent) return nullptr;
        IUnknownPtr pUnk = pParent->item[full.substr(cut + 1).c_str()].GetUnknown();
        if (!pUnk) return nullptr;
        IWzCanvasPtr cv;
        if (FAILED(pUnk.QueryInterface(__uuidof(IWzCanvas), &cv))) return nullptr;
        return cv;
    } catch (...) {
        return nullptr;
    }
}

// max |a-b| across A, R, G and B.
inline int ChannelDelta(uint32_t a, uint32_t b) {
    int d = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        const int x = std::abs(static_cast<int>((a >> sh) & 0xFF) -
                               static_cast<int>((b >> sh) & 0xFF));
        if (x > d) d = x;
    }
    return d;
}

inline bool IsChromatic(uint32_t argb) {
    if (((argb >> 24) & 0xFF) == 0) return false;
    float h, s, v;
    RgbToHsv((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF, h, s, v);
    return s >= kIrisMinSat && v >= kIrisMinVal;
}

const std::vector<FaceIrisPlan>& FacePlansFor(int faceId) {
    auto it = g_facePlans.find(faceId);
    if (it != g_facePlans.end()) return it->second;

    std::vector<FaceIrisPlan> plans(_countof(kFaceIrisPaths));
    const int base = faceId - 100 * ((faceId / 100) % 10);

    IWzPropertyPtr pWornImg = FaceImg(faceId);
    IWzPropertyPtr pRefImg[3];
    for (int r = 0; r < 3; ++r) pRefImg[r] = FaceImg(base + kFaceRefOffsets[r]);

    if (pWornImg && pRefImg[0] && pRefImg[1] && pRefImg[2]) {
        for (size_t i = 0; i < _countof(kFaceIrisPaths); ++i) {
            const wchar_t* path = kFaceIrisPaths[i];
            int w = 0, h = 0;
            std::vector<uint32_t> worn, ref[3];
            if (!ReadCanvasPixels(CanvasAtPath(pWornImg, path), w, h, worn)) continue;

            bool ok = true;
            for (int r = 0; r < 3 && ok; ++r) {
                int rw = 0, rh = 0;
                // Size equality is REQUIRED, not nice to have: 7 of 193 vanilla styles
                // ship a variant at a different size (and a different map/brow), and
                // indexing across those would scramble the mask. Skipping costs almost
                // nothing and cannot produce a wrong pixel.
                ok = ReadCanvasPixels(CanvasAtPath(pRefImg[r], path), rw, rh, ref[r]) &&
                     rw == w && rh == h;
            }
            if (!ok) continue;

            const size_t n = worn.size();
            std::vector<uint8_t> mask(n, 0);
            size_t hits = 0;
            for (size_t k = 0; k < n; ++k) {
                // Parenthesised: <windows.h> defines max() as a macro and would eat these.
                const int d = (std::max)(ChannelDelta(ref[0][k], ref[1][k]),
                              (std::max)(ChannelDelta(ref[0][k], ref[2][k]),
                                         ChannelDelta(ref[1][k], ref[2][k])));
                if (d < kIrisChannelDelta) continue;
                if (!IsChromatic(ref[0][k]) && !IsChromatic(ref[1][k]) && !IsChromatic(ref[2][k]))
                    continue;
                mask[k] = 1;
                ++hits;
            }
            if (!hits) continue;                 // nothing varies: not an iris canvas
            plans[i].w = w;
            plans[i].h = h;
            plans[i].mask.swap(mask);
            plans[i].ref = ref[0];               // Red: the colour the slider rotates FROM
        }
    }

    // ALL OR NOTHING, keyed on default/face. A partial plan is worse than none: if the
    // resting canvas is skipped but blink/wink/hit are not, the eyes sit their original
    // colour and then FLASH the tint mid-blink. That is not hypothetical -- face 20100
    // (style 00, colour 1) ships its default/face at 26x17 while its colour siblings are
    // 26x16, so the size guard drops exactly that one canvas and keeps the other four.
    //
    // A canvas whose own mask is legitimately empty is a different case and is left
    // alone: those are frames with the eyes CLOSED, which carry no iris to tint, so
    // nothing flickers.
    if (plans[0].Empty()) {
        for (FaceIrisPlan& p : plans) {
            p.mask.clear();
            p.ref.clear();
            p.w = p.h = 0;
        }
    }

    bool anyPlan = false;
    for (const FaceIrisPlan& p : plans) anyPlan = anyPlan || !p.Empty();
    if (!anyPlan) {
        // ~6% of installed face ids: an imported family duplicated one file into
        // several colour slots, so the siblings are byte-identical and there is
        // nothing to diff. Tinting the whole canvas instead is NOT an acceptable
        // fallback (see the header of this section), so the eye tint is simply a
        // no-op for those styles and says so once.
        LOG_ONCE("weapontint: face %d has no usable colour siblings; eye tint is "
                 "inert for this style", faceId);
    }
    return g_facePlans.emplace(faceId, std::move(plans)).first->second;
}

// Clone `pSrc` with only the masked pixels rewritten, sourced from the plan's Red
// reference and then rotated. Everything outside the mask keeps the WORN variant's
// own bytes -- which is what stops a style whose Black variant has brown eyebrows
// (00020020.img: 38 px, worst channel delta 204) from having them turned grey.
IWzCanvasPtr CloneIrisTinted(IWzCanvasPtr pSrc, const FaceIrisPlan& plan,
                             const WeaponTint& t) {
    if (!pSrc || plan.Empty()) return nullptr;
    IWzCanvasPtr dst;
    try {
        const int w = static_cast<int>(pSrc->width);
        const int h = static_cast<int>(pSrc->height);
        if (w != plan.w || h != plan.h) return nullptr;

        PcCreateObject<IWzCanvasPtr>(L"Canvas", dst, nullptr);
        if (!dst) return nullptr;
        dst->Create(w, h, 0, CP_A8R8G8B8);
        dst->CopyEx(0, 0, pSrc, CANVAS_ALPHATYPE::CA_OVERWRITE, 0, 0, 0, 0, 0, 0);
        if (!CopyCanvasProperty(pSrc, dst)) return nullptr;

        std::vector<uint32_t> px(static_cast<size_t>(w) * h, 0);
        if (!ReadPixelsLocked(dst, w, h, px.data())) ReadPixelsSlow(dst, w, h, px.data());

        for (size_t k = 0; k < px.size() && k < plan.mask.size(); ++k) {
            if (!plan.mask[k]) continue;
            uint32_t v = plan.ref[k];
            TintPixel(v, t);
            px[k] = v;
        }
        WriteRuns(dst, w, h, px.data());
        dst->cx = pSrc->cx;
        dst->cy = pSrc->cy;
    } catch (...) {
        return nullptr;
    }
    return dst;
}

IWzCanvasPtr GetIrisTinted(IWzCanvasPtr pSrc, const FaceIrisPlan& plan, const WeaponTint& t) {
    if (!pSrc) return nullptr;
    if (!g_irisClonesValid || g_irisClonesKey != t.Key()) {
        g_irisClones.clear();
        g_irisClonesKey = t.Key();
        g_irisClonesValid = true;
    }
    IWzCanvas* raw = pSrc;
    auto it = g_irisClones.find(raw);
    if (it != g_irisClones.end()) return it->second;
    IWzCanvasPtr clone = CloneIrisTinted(pSrc, plan, t);
    g_irisClones.emplace(raw, clone);    // cache failures too, so they are not retried per frame
    return clone;
}

// Swap the iris-bearing canvases of the worn face for masked clones.
void SwapInFaceTintInto(int faceId, const WeaponTint& t, std::vector<Swap>& swaps) {
    if (faceId <= 0) return;
    const std::vector<FaceIrisPlan>& plans = FacePlansFor(faceId);
    IWzPropertyPtr pImg = FaceImg(faceId);
    if (!pImg) return;
    int swapped = 0;
    try {
        for (size_t i = 0; i < _countof(kFaceIrisPaths) && i < plans.size(); ++i) {
            if (plans[i].Empty()) continue;
            // Split the path so the PARENT can be recorded for the swap-back.
            std::wstring full = kFaceIrisPaths[i];
            const size_t cut = full.find_last_of(L'/');
            const std::wstring leaf = full.substr(cut + 1);
            IWzPropertyPtr pParent = (cut == std::wstring::npos)
                ? pImg : PropertyAtPath(pImg, full.substr(0, cut));
            if (!pParent) continue;

            IWzCanvasPtr pOrig;
            IUnknownPtr pUnk = pParent->item[leaf.c_str()].GetUnknown();
            if (!pUnk || FAILED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pOrig)) || !pOrig)
                continue;

            IWzCanvasPtr pTinted = GetIrisTinted(pOrig, plans[i], t);
            if (!pTinted) continue;
            swaps.push_back(Swap{ pParent, leaf, pOrig });
            pParent->item[leaf.c_str()] = static_cast<IUnknown*>(pTinted);
            ++swapped;
        }
    } catch (...) {
    }
    LOG_ONCE_PER_ID(faceId,
        "weapontint: face %d iris path: %d canvases swapped (h=%d c=%d v=%d)",
        faceId, swapped, t.hue, static_cast<int>(t.chroma), static_cast<int>(t.bright));
}

void SwapInFaceTint(int faceId, const WeaponTint& t) {
    SwapInFaceTintInto(faceId, t, g_swaps);
}

// The face assembler keeps a direct reference to its source IWzCanvas.  Replacing
// the canvas in the parent property (which is enough for equip layers) therefore
// does not affect a face frame once that reference has been cached.  For the narrow
// native face-build call, repaint that exact source canvas instead, then put every
// changed pixel back before any other avatar can render from it.
struct FaceCanvasPatch {
    IWzCanvasPtr canvas;
    int w = 0, h = 0;
    std::vector<uint32_t> original;
};

void RestoreFacePatches(std::vector<FaceCanvasPatch>& patches) {
    for (auto it = patches.rbegin(); it != patches.rend(); ++it) {
        if (it->canvas && it->w > 0 && it->h > 0 && !it->original.empty())
            WriteRuns(it->canvas, it->w, it->h, it->original.data());
    }
    patches.clear();
}

void PaintFaceIrisForBuild(int faceId, const WeaponTint& t,
                           std::vector<FaceCanvasPatch>& patches) {
    if (faceId <= 0) return;
    const std::vector<FaceIrisPlan>& plans = FacePlansFor(faceId);
    IWzPropertyPtr image = FaceImg(faceId);
    if (!image) return;
    for (size_t i = 0; i < plans.size() && i < _countof(kFaceIrisPaths); ++i) {
        const FaceIrisPlan& plan = plans[i];
        if (plan.Empty()) continue;
        IWzCanvasPtr canvas = CanvasAtPath(image, kFaceIrisPaths[i]);
        int w = 0, h = 0;
        std::vector<uint32_t> before;
        if (!ReadCanvasPixels(canvas, w, h, before) || w != plan.w || h != plan.h)
            continue;
        std::vector<uint32_t> after = before;
        bool changed = false;
        for (size_t px = 0; px < after.size() && px < plan.mask.size(); ++px) {
            if (!plan.mask[px]) continue;
            uint32_t tintPixel = plan.ref[px];
            TintPixel(tintPixel, t);
            if (after[px] != tintPixel) {
                after[px] = tintPixel;
                changed = true;
            }
        }
        if (!changed || WriteRuns(canvas, w, h, after.data()) <= 0) continue;
        FaceCanvasPatch patch;
        patch.canvas = canvas;
        patch.w = w; patch.h = h;
        patch.original.swap(before);
        patches.push_back(std::move(patch));
    }
}

struct FacePatchGuard {
    std::vector<FaceCanvasPatch> patches;
    ~FacePatchGuard() { RestoreFacePatches(patches); }
};

// Face.img has eight artist-authored eye colours per style.  The face assembler
// caches its original canvas before a WZ-node replacement can reach it, but it does
// honour the face id it is handed.  Use the closest native colour variant while the
// layer is built; this gives the rendered layer a genuinely different source rather
// than trying to edit the already-cached source canvas.
int NativeFaceForTint(int faceId, const WeaponTint& tint) {
    const int base = faceId - 100 * ((faceId / 100) % 10);
    int variant = 2; // red / pink, the reference hue for the Prism's hue slider
    const int h = tint.hue;
    if (tint.bright <= -70 || tint.chroma <= -75) {
        variant = 0; // black
    } else if (h < 30 || h >= 330) {
        variant = 2; // red / pink
    } else if (h < 80) {
        variant = 4; // brown / amber
    } else if (h < 155) {
        variant = 3; // green
    } else if (h < 210) {
        variant = 5; // aqua
    } else if (h < 265) {
        variant = 1; // blue
    } else if (h < 320) {
        variant = 6; // purple
    } else {
        variant = 7; // magenta
    }
    const int candidate = base + variant * 100;
    return FaceImg(candidate) ? candidate : faceId;
}

struct FaceIdGuard {
    CAvatar* avatar = nullptr;
    int original = 0;
    FaceIdGuard(CAvatar* a, int replacement) : avatar(a), original(a->m_avatarLook.nFace) {
        a->m_avatarLook.nFace = replacement;
    }
    ~FaceIdGuard() { if (avatar) avatar->m_avatarLook.nFace = original; }
};

// SKIN. nSkin is a small ordinal, not an item id, and the skin is TWO imgs, not one:
//
//   Character/000020NN.img   NN = 2000 + nSkin    body, arm, hand, lHand, rHand, armOverHair
//   Character/000120NN.img   NN = 12000 + nSkin   head
//
// Tinting only the first one dyes the body and leaves the head its original colour, which
// looks like the feature half working rather than like a missing img.
//
// BOTH ARE WALKED PER ACTION, FROM A CACHED PLAN. They used to be walked whole on every
// build, on the belief that the body held 258 canvases. This Data tree's skin imgs are the
// post-import ones: 00002000.img alone has 2774 canvases, 656 linked canvases and some
// 15,000 sub-properties under ~400 actions. Walking that through COM on every
// PrepareActionLayer froze the client for seconds -- on every slider step in the preview,
// and then on every equip or action change for as long as a skin tint was applied.
//
// A build only ever reads the action being built, so that is all that is planned, plus
// whatever that action REDIRECTS to. The body img is the client's action table, and a
// redirecting frame holds no canvases, only `<action>/<frame>/action = "<other action>"`
// (alert2 -> alert, savage -> stabO1, ...). Those are followed by name, transitively,
// because the body drew vanilla through every one of them when they were not. UOL aliases
// into other actions (prone/0/body -> proneStab/0/body) need nothing extra: the plan
// records the alias slot itself and restores the alias, not the canvas behind it.
//
// Dyeing the canvases is deliberate rather than swapping nSkin to another skin id: the client
// ships a fixed set of skins, and the point of the tab is a free colour.
void CollectActionPlan(IWzPropertyPtr pImg, const std::wstring& action, SwapPlan& plan,
                       std::vector<std::wstring>& seen) {
    if (!pImg || action.empty() || seen.size() >= 16) return;
    if (std::find(seen.begin(), seen.end(), action) != seen.end()) return;
    seen.push_back(action);
    try {
        Ztl_variant_t v = pImg->item[action.c_str()];
        IUnknownPtr pUnk = get_unknown(v);
        IWzPropertyPtr pAct;
        if (!pUnk || FAILED(pUnk.QueryInterface(__uuidof(IWzProperty), &pAct)) || !pAct) return;
        BuildPlanFrom(pAct, plan, 1, false);
        for (const std::wstring& f : ChildNames(pAct)) {
            Ztl_variant_t vf = pAct->item[f.c_str()];
            IUnknownPtr pf = get_unknown(vf);
            IWzPropertyPtr pFrame;
            if (!pf || FAILED(pf.QueryInterface(__uuidof(IWzProperty), &pFrame)) || !pFrame) continue;
            std::wstring target;
            try {
                Ztl_variant_t va = pFrame->item[L"action"];
                if (V_VT(&va) == VT_BSTR && V_BSTR(&va)) target = V_BSTR(&va);
            } catch (...) {
            }
            if (!target.empty()) CollectActionPlan(pImg, target, plan, seen);
        }
    } catch (...) {
    }
}

// Keyed on the LIVE img pointer (same invalidation rule as PlanFor) plus the action name.
std::map<std::pair<IWzProperty*, std::wstring>, std::shared_ptr<SwapPlan>> g_actionPlans;

std::shared_ptr<SwapPlan> ActionPlanFor(IWzPropertyPtr pImg, const wchar_t* action) {
    if (!pImg || !action || !*action) return nullptr;
    auto key = std::make_pair(pImg.GetInterfacePtr(), std::wstring(action));
    auto it = g_actionPlans.find(key);
    if (it != g_actionPlans.end()) return it->second;
    auto plan = std::make_shared<SwapPlan>();
    std::vector<std::wstring> seen;
    CollectActionPlan(pImg, key.second, *plan, seen);
    g_actionPlans.emplace(key, plan);
    LOG_ONCE_PER_ID(static_cast<int>(plan->entries.size()) * 1000 + static_cast<int>(seen.size()),
                    "weapontint: action plan %ls: %d slots over %d action(s)", action,
                    static_cast<int>(plan->entries.size()), static_cast<int>(seen.size()));
    return plan;
}

void SwapInSkinTint(int skinId, const wchar_t* actionName, const WeaponTint& t) {
    if (skinId < 0) return;
    try {
        wchar_t path[80];
        // Body: body, arm, hand, lHand, rHand, armOverHair of the action being built.
        if (actionName) {
            _snwprintf_s(path, _countof(path), _TRUNCATE, L"Character/%08d.img", 2000 + skinId);
            IWzPropertyPtr pBody = get_rm()->GetObjectA(path).GetUnknown();
            if (pBody) ApplyPlan(ActionPlanFor(pBody, actionName), t, Layer::Body);
        }
        // Head. Shaped like hair: the stock actions' frames are all UOLs into the two real
        // canvases at front/head and back/head, so those two are always planned; the action
        // plan then adds the few actions that carry real canvases of their own (the PB*
        // set). EVERY plan is built before ANY is applied: a plan must be recorded from the
        // tree at rest, and once front/head holds a clone an alias read through it would
        // record the clone as the "original".
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"Character/%08d.img", 12000 + skinId);
        IWzPropertyPtr pHead = get_rm()->GetObjectA(path).GetUnknown();
        if (pHead) {
            std::shared_ptr<SwapPlan> plans[3];
            int n = 0;
            for (const wchar_t* side : { L"front", L"back" }) {
                Ztl_variant_t v = pHead->item[side];
                IUnknownPtr pUnk = get_unknown(v);
                IWzPropertyPtr pSide;
                if (pUnk && SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzProperty), &pSide)) && pSide)
                    plans[n++] = PlanFor(pSide);
            }
            if (actionName) plans[n++] = ActionPlanFor(pHead, actionName);
            for (int i = 0; i < n; ++i) ApplyPlan(plans[i], t, Layer::Body);
        }
    } catch (...) {
    }
}

// Hair: the real canvases, reached through the CURRENT ACTION plus `default` and
// `backDefault`.
//
// THE ACTION NODE IS NOT OPTIONAL, though it reads as though it should be: most styles
// alias every per-action frame back into `default`, and this walked only `default` and
// `backDefault` on that basis. Measured over all 17483 hair imgs that have a prone node,
// 12481 do alias `prone/0/hair` into `default/hair` -- and 5002 do not:
//     2806  a separate artist-drawn lying-down canvas, stored INLINE under prone/0
//     1988  an alias across to proneStab/0/hair
//       16  an alias to a top-level `defaultProne` node
//      192  somewhere else again
// None of those is reachable from `default`, so the hair went vanilla the moment the
// character lay down while standing hair stayed dyed. Walking the action node covers the
// inline case directly and the alias cases through the alias slot itself.
void SwapInHairTint(int hairId, const wchar_t* /*actionName*/, const WeaponTint& t) {
    if (hairId <= 0) return;
    try {
        wchar_t path[80];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"Character/Hair/%08d.img", hairId);
        IWzPropertyPtr pImg = get_rm()->GetObjectA(path).GetUnknown();
        if (!pImg) return;
        ApplyPlan(PlanFor(pImg), t, Layer::Body);
    } catch (...) {
    }
}

void SwapOutList(std::vector<Swap>& swaps) {
    for (auto it = swaps.rbegin(); it != swaps.rend(); ++it) {
        try {
            if (it->parent && it->original) {
                it->parent->item[it->name.c_str()] = static_cast<IUnknown*>(it->original);
            }
        } catch (...) {
        }
    }
    swaps.clear();
}

void SwapOut() {
    SwapOutList(g_swaps);
}

// =====================================================
// WHOSE AVATAR IS THIS
// =====================================================
CAvatar* LocalAvatar() {
    auto* pUser = *reinterpret_cast<unsigned char**>(kAddr_CUserLocal_Instance);
    if (!pUser) return nullptr;
    return reinterpret_cast<CAvatar*>(pUser + kOff_AvatarInUser);
}

// Character name of the CUser an avatar belongs to, or "" .
//
// SAFETY: not every CAvatar has a CUser in front of it -- the prism window builds
// its own, and character select has others -- so `pAvatar - 0x88` can be arbitrary
// memory. Every read is SEH-guarded and the result must look like a MapleStory name
// (printable ASCII, 3..13 chars; Cosmic accepts 3..12) to be returned at all. A junk read therefore
// yields "", which matches no entry in the tint table and simply means "no tint" --
// never a wrong colour.
bool NameOfAvatar(CAvatar* pAvatar, char* out, size_t cap) {
    if (!pAvatar || cap < 16) return false;
    out[0] = '\0';
    int len = 0;
    __try {
        auto* pUser = reinterpret_cast<unsigned char*>(pAvatar) - kOff_AvatarInUser;
        const char* p = *reinterpret_cast<char* const*>(pUser + kOff_CharacterNameInUser);
        if (reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
        while (len < 13) {
            const char c = p[len];
            if (c == '\0') break;
            if (c < 0x20 || c > 0x7E) return false;
            out[len++] = c;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (len < 3) return false;      // shortest name Cosmic accepts (3..12)
    out[len] = '\0';
    return true;
}

// The worn Cash-weapon id off an avatar's look, filtered by the range the client
// actually honours: the equip-slot loop gates the sticker on `id / 100000 == 17`
// at 0x004129BE, so an id outside 17xxxxx never reaches Character/Weapon/.
//
// Only the WINDOW uses this, as the default target until an item is dropped on it;
// the render path takes every worn item from WornItemsOf instead.
//
// SEH leaf: `pAvatar` may not be an avatar at all.
int CashWeaponOf(CAvatar* pAvatar) {
    if (!pAvatar) return 0;
    int id = 0;
    __try { id = pAvatar->m_avatarLook.nWeaponStickerID; }
    __except (EXCEPTION_EXECUTE_HANDLER) { id = 0; }
    return (id > 0 && id / 100000 == 17) ? id : 0;
}

// Which tints apply to `pAvatar`, resolved per ITEM.
//
//   * forced scope  -- a preview avatar this DLL built
//   * local avatar  -- the player's own
//   * anything else -- looked up by character name in the map table
// In the first two cases the prism window's live slider values override the stored
// tint for the ONE item being dyed, so the rest of the outfit keeps its own colours
// while you drag.
struct TintScope {
    const TintMap* stored = nullptr;
    const TintMap* forced = nullptr;
    bool  preview = false;      // apply the window's live values on top
    bool  any = false;

    bool Lookup(int itemId, WeaponTint& out) const {
        if (preview && itemId == g_previewItemId) { out = g_preview; return true; }
        // A saved salon look may intentionally contain the identity tint. Check this
        // before the player's normal table so a neutral saved look does not inherit
        // their currently worn hair/eye colour in its thumbnail.
        if (forced) {
            auto it = forced->find(itemId);
            if (it != forced->end()) { out = it->second; return true; }
        }
        if (!stored) return false;
        auto it = stored->find(itemId);
        if (it == stored->end()) return false;
        out = it->second;
        return true;
    }
};

TintScope ScopeForAvatar(CAvatar* pAvatar) {
    TintScope s;
    const bool isForced = (g_forcedScope > 0
            && (g_forcedAvatar == nullptr || pAvatar == g_forcedAvatar));
    const bool isLocal  = pAvatar == LocalAvatar();
    if (isLocal) {
        std::lock_guard<std::mutex> lock(g_mtx);
        s.stored = &g_saved;
        // THE LIVE PREVIEW IS SCOPED TO THE WINDOW'S OWN AVATAR. Only an avatar this DLL
        // built inside a forced scope sees the slider values; the character standing in
        // the world keeps its SAVED colours until Confirm. Dragging a slider used to
        // recolour both at once, which made it impossible to compare a candidate colour
        // against what you are actually wearing -- and left the world character wearing a
        // colour that was never bought if the window was then cancelled.
        s.any = !g_saved.empty();
        return s;
    }

    if (pAvatar != nullptr && pAvatar == g_prismPreviewAvatar) {
        std::lock_guard<std::mutex> lock(g_mtx);
        s.stored = &g_saved;
        s.preview = g_previewActive;
        s.any = s.preview || !g_saved.empty();
        return s;
    }

    if (isForced) {
        std::lock_guard<std::mutex> lock(g_mtx);
        s.stored = &g_saved;
        s.forced = &g_forcedTints;
        // An unbound legacy scope belongs to another preview window and must not
        // consume the Coloring Prism's live slider state.
        s.preview = g_forcedAvatar != nullptr && g_previewActive;
        s.any = s.preview || !g_forcedTints.empty() || !g_saved.empty();
        return s;
    }

    char name[16];
    if (!NameOfAvatar(pAvatar, name, sizeof(name))) return s;

    std::lock_guard<std::mutex> lock(g_mtx);
    // Remember which avatar wears this name so a later tint change can repaint it
    // without a user-pool walk (this repo has no way to enumerate CUsers).
    g_avatarByName[name] = pAvatar;
    auto it = g_remoteTints.find(name);
    if (it == g_remoteTints.end() || it->second.empty()) return s;
    s.stored = &it->second;
    s.any = true;
    return s;
}

// Puts the WZ tree back however the guarded scope exits -- including through a
// C++ exception thrown out of the client's own COM wrappers. Leaving a tinted
// clone behind in a ResMan-owned node would be a permanent, process-global
// recolor of that weapon for every character and every tooltip.
struct SwapGuard {
    ~SwapGuard() { SwapOut(); }
};

// =====================================================
// CASH EFFECT ITEMS
// =====================================================
// The 5010000..5019999 group: items that live in the Cash tab of the item inventory and
// play an effect around the character. Their art is
// Item/Cash/0501.img/<id>/effect/<posture>/<frame>, which is neither an avatar action
// layer nor an ItemEff layer, so neither hook above ever sees it.
//
// The client builds it in two small functions, both __thiscall(CUser*, int nItemID)
// ending `ret 4`, sharing a `[this+0x568]` guard and an `idiv 10000` on the argument to
// dispatch on the item group. They were found by scanning for the __SEH_prolog4 entry
// (`mov eax, <scopetable>; call 0x00A60B98`) that this whole region uses instead of the
// usual `push ebp; mov ebp, esp`, which is why an ordinary prologue scan misses them.
//
// The ITEM ID IS THE ARGUMENT, which is what makes this cheap: no packet to decode and
// no state to track, the hook is handed exactly the key the swap needs.
std::vector<Swap> g_cashEffSwaps;

bool BeginCashEffectSwap(CAvatar* pAvatar, int itemId) {
    SwapOutList(g_cashEffSwaps);            // a previous build that never got closed
    if (!pAvatar || itemId <= 0) return false;
    const TintScope scope = ScopeForAvatar(pAvatar);
    if (!scope.any) return false;
    WeaponTint t;
    // The EFFECT key, the one the window's Effects tab writes. A cash effect IS the
    // effect: there is no body half to keep apart, so it needs no second key.
    if (!scope.Lookup(EffectTintKeyFor(itemId), t) || t.IsIdentity()) return false;
    try {
        // The WHOLE `effect` subtree, for the reason the ItemEff swap gives: 346 of the
        // posture nodes in this group are UOLs into a sibling, so resolving a single
        // posture can land on an indirection with no canvases under it and tint nothing.
        // Affordable because these are small: a median of 19 canvases per entry.
        wchar_t path[80];
        _snwprintf_s(path, _countof(path), _TRUNCATE,
                     L"Item/Cash/0501.img/%08d/effect", itemId);
        IWzPropertyPtr pNode = get_rm()->GetObjectA(path).GetUnknown();
        if (!pNode) return false;
        SwapSinkTo sink(g_cashEffSwaps);
        SwapSubtree(pNode, t, 0, Layer::Body);
    } catch (...) {
        // Whatever landed in g_cashEffSwaps is still undone by the guard below.
    }
    return !g_cashEffSwaps.empty();
}

// Restores however the guarded call exits, including through a _com_error raised out of
// the client's own COM wrappers. Leaving a tinted clone in a ResMan-owned node would be a
// permanent, process-global recolour of that effect for every character.
struct CashEffectSwapGuard {
    ~CashEffectSwapGuard() { SwapOutList(g_cashEffSwaps); }
};

CAvatar* AvatarOfUser(void* pUser) {
    return reinterpret_cast<CAvatar*>(reinterpret_cast<unsigned char*>(pUser)
                                      + kOff_AvatarInUser);
}

using t_CashEffect = void(__thiscall*)(void*, int);
auto CashEffectShow_Orig  = reinterpret_cast<t_CashEffect>(0x0093BEB9);
auto CashEffectApply_Orig = reinterpret_cast<t_CashEffect>(0x0093C218);

// WHAT THE CLIENT LAST SHOWED, so a colour-only change can be made visible.
//
// These builders run when the client is TOLD to show an effect, which a recolour never
// does: the item is unchanged, only its canvases are a different colour, so the layer the
// client already built keeps the old ones until something re-shows it. Re-equipping is
// what players were having to do. Remembering the last call lets the tick replay it.
//
// The FUNCTION is remembered too, not just the arguments. The two builders are not
// interchangeable, and replaying the wrong one could attach a second layer rather than
// rebuild the one on screen.
void* g_lastCashEffectUser = nullptr;
int   g_lastCashEffectItem = 0;
bool  g_lastCashEffectWasApply = false;

void __fastcall CashEffectShow_Hook(void* pThis, void* /*edx*/, int nItemID) {
    CashEffectSwapGuard guard;
    BeginCashEffectSwap(AvatarOfUser(pThis), nItemID);
    CashEffectShow_Orig(pThis, nItemID);
    g_lastCashEffectUser = pThis;
    g_lastCashEffectItem = nItemID;
    g_lastCashEffectWasApply = false;
}

void __fastcall CashEffectApply_Hook(void* pThis, void* /*edx*/, int nItemID) {
    CashEffectSwapGuard guard;
    BeginCashEffectSwap(AvatarOfUser(pThis), nItemID);
    CashEffectApply_Orig(pThis, nItemID);
    g_lastCashEffectUser = pThis;
    g_lastCashEffectItem = nItemID;
    g_lastCashEffectWasApply = true;
}

// Replay the local player's last cash-effect build so it picks the new colour up.
//
// Scoped to the LOCAL user on purpose. A remote player's effect is rebuilt by the same
// hooks whenever the server re-announces it, and replaying a build for someone else off a
// pointer we merely remember would be a use-after-free the moment they left the map.
void ReshowLocalCashEffect() {
    if (!g_lastCashEffectItem || !g_lastCashEffectUser) return;
    CAvatar* pLocal = LocalAvatar();
    if (!pLocal) return;
    void* pUser = reinterpret_cast<unsigned char*>(pLocal) - kOff_AvatarInUser;
    if (pUser != g_lastCashEffectUser) return;      // stale, or never the local player
    // Through the HOOK, not the original, so the swap is open while it rebuilds.
    if (g_lastCashEffectWasApply) CashEffectApply_Hook(pUser, nullptr, g_lastCashEffectItem);
    else                          CashEffectShow_Hook(pUser, nullptr, g_lastCashEffectItem);
}

// =====================================================
// THE HOOK
// =====================================================
using t_PrepareActionLayer = void(__thiscall*)(void*, int, int, int);
auto PrepareActionLayer_Orig =
    reinterpret_cast<t_PrepareActionLayer>(kAddr_CAvatar_PrepareActionLayer);

// Force the avatar to rebuild its action layers so a tint change shows NOW rather
// than at the next action change.
//
// THE CACHE IS THE WHOLE PROBLEM. PrepareActionLayer keeps the built frames per
// action and refuses to build them twice -- byte-verified on its normal path at
// 0x004545C3:
//     mov eax,[ebp-0x14]   ; layerSet + action*4 + 0x10, the cached frame array
//     mov eax,[eax]
//     cmp eax,esi
//     je  0x004545D5       ;   empty -> fall through and build
//     cmp [eax-4],esi
//     jne 0x00454660       ;   non-empty -> SKIP the CActionMan frame build
// so once the player has stood in a given action once, asking for a reload reads
// nothing out of the WZ tree and the swapped-in tinted canvases are never seen.
// That is why a tint used to appear only after unequipping and re-equipping the
// weapon (which clears the layers), and why the prism's own preview avatar tinted
// correctly all along -- it is a fresh CAvatar whose caches are empty.
//
// So the cache is dropped first, for BOTH 0x5DC layer sets, and only then is the
// reload issued.
//
// CAvatar::SetMoveAction(nMA, bReload) is the client's own reload entry, and
// bReload is exactly the flag for this. Byte-verified at 0x004520F1:
//     cmp [esp+8],0        ; bReload
//     mov eax,[esp+4]      ; nMA
//     jne 0x0045210B       ; bReload != 0 -> SKIP the early-out below
//     cmp eax,[esi+0x4E8]  ; nMA == m_nMoveAction ?
//     je  0x00452191       ;   -> return, nothing to do
//     ...
//     push 0 / push 0x64 / push 6 / call [eax+0x14]   ; PrepareActionLayer(6,100,0)
// so passing the CURRENT move action with bReload=1 rebuilds without changing the
// animation.
//
// The previous attempt called CUser::OnAvatarModified instead. That does nothing
// here: the tint is not part of AvatarLook, so the look is byte-identical to the
// copy the client keeps at CAvatar+0x1C9 and it correctly concludes there is
// nothing to rebuild. Unequip/reequip DID repaint because that genuinely changes
// the look -- which is precisely why the bug looked like a refresh problem rather
// than a render one.
//
// CAvatar::SetMoveAction is called directly rather than through vtable slot 4, so
// CUser's override does not run: that one also reports the move action onward, and
// a purely cosmetic repaint has no business generating traffic.
void SehReloadActionLayer(CAvatar* pAvatar) {
    __try {
        auto* p = reinterpret_cast<unsigned char*>(pAvatar);
        const int nMA = *reinterpret_cast<int*>(p + kOff_MoveAction);
        auto ClearActionLayer =
            reinterpret_cast<void(__thiscall*)(void*, int)>(kAddr_CAvatar_ClearActionLayer);
        ClearActionLayer(pAvatar, 0);
        ClearActionLayer(pAvatar, 1);
        reinterpret_cast<void(__thiscall*)(void*, int, int)>(kAddr_CAvatar_SetMoveAction)(
            pAvatar, nMA, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// SEH leaf (C2712: __try may not share a function with objects that unwind).
int CurrentActionOf(CAvatar* pAvatar) {
    int action = -1;
    __try { action = pAvatar->GetCurrentAction(nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { action = -1; }
    return action;
}

// Every item this avatar is currently SHOWING, into `out`. anHairEquip already
// resolves cash-over-normal per slot, so it is the visible item in every case
// except the weapon -- slot 11 is deliberately excluded from that override and the
// Cash weapon lives separately in nWeaponStickerID (verified in the look builder at
// 0x004E7358). Both are collected here, so a player wearing a Cash hat over a
// normal one contributes only the hat that is actually drawn.
//
// SEH leaf: raw reads through a pointer that may not be an avatar at all.
int WornItemsOf(CAvatar* pAvatar, int* out, int cap) {
    int n = 0;
    __try {
        const int sticker = pAvatar->m_avatarLook.nWeaponStickerID;
        if (sticker > 0 && sticker / 100000 == 17 && n < cap) out[n++] = sticker;
        for (int slot = 0; slot < kAvatarEquipSlots && n < cap; ++slot) {
            const int id = pAvatar->m_avatarLook.anHairEquip[slot];
            if (id > 0) out[n++] = id;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

// Hair and face straight off an avatar's look. Hair is anHairEquip[0] (equip slot 0,
// the same 0x19 coloringprism.cpp's kAL_HairEquip0 names), face is the standalone
// nFace at +0x11.
//
// SEH leaf: raw reads through a pointer that may not be an avatar at all, so a junk
// read must yield 0/0 -- "no look tint" -- rather than fault mid-render.
bool SehLookIdsOf(CAvatar* pAvatar, int& hairId, int& faceId, int& skinId) {
    hairId = 0;
    faceId = 0;
    skinId = -1;                       // -1, not 0: skin 0 is a real skin
    __try {
        hairId = pAvatar->m_avatarLook.anHairEquip[0];
        faceId = pAvatar->m_avatarLook.nFace;
        skinId = pAvatar->m_avatarLook.nSkin;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hairId = 0;
        faceId = 0;
        skinId = -1;
        return false;
    }
    return true;
}

// The NORMAL weapon this avatar is wearing, which is what a Cash weapon's art is filed
// under. anHairEquip resolves cash-over-normal per slot for everything EXCEPT the weapon,
// so slot 11 still holds the base weapon even when a sticker is drawn over it.
//
// SEH leaf: raw read through a pointer that may not be an avatar at all.
int SehBaseWeaponIdOf(CAvatar* pAvatar) {
    int id = 0;
    __try {
        id = pAvatar->m_avatarLook.anHairEquip[11];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return id;
}

// The callers that only want hair and face keep the old shape.
bool SehLookIdsOf(CAvatar* pAvatar, int& hairId, int& faceId) {
    int skinDiscard = -1;
    return SehLookIdsOf(pAvatar, hairId, faceId, skinDiscard);
}

void __fastcall PrepareActionLayer_Hook(void* pThis, void* /*edx*/,
                                        int nActionSpeed, int nWalkSpeed, int bKeyDown) {
    auto* pAvatar = reinterpret_cast<CAvatar*>(pThis);

    int hairId = 0, faceId = 0, skinId = -1;
    SehLookIdsOf(pAvatar, hairId, faceId, skinId);
    const TintScope scope = ScopeForAvatar(pAvatar);
    if (!scope.any) {
        PrepareActionLayer_Orig(pThis, nActionSpeed, nWalkSpeed, bKeyDown);
        return;
    }
    const int action = CurrentActionOf(pAvatar);
    if (action < 0) {
        PrepareActionLayer_Orig(pThis, nActionSpeed, nWalkSpeed, bKeyDown);
        return;
    }

    int items[kAvatarEquipSlots + 1];
    const int count = WornItemsOf(pAvatar, items, _countof(items));

    // The hair and face ids come off THIS avatar's own look rather than from the
    // server, because for a remote player that look is the only correct source --
    // the table only says what colour to apply, never to what.
    Ztl_bstr_t sAction;
    get_action_name_from_code(&sAction, action);
    // Which weapon-type subtree a Cash weapon's art is filed under. -1 means "unknown",
    // which walks all of them rather than risking a weapon that does not recolour.
    const int weaponTypeCode = WeaponTypeCodeOf(SehBaseWeaponIdOf(pAvatar));

    {
        SwapGuard guard;                       // restores the tree on ANY exit path
        g_cost = SwapCost{};
        LARGE_INTEGER tSwap0, tSwap1, tFreq;
        QueryPerformanceFrequency(&tFreq);
        QueryPerformanceCounter(&tSwap0);
        for (int i = 0; i < count; ++i) {
            // anHairEquip[0] IS the hair id, so it arrives here as a worn "item" too.
            // It must not go through the equip path: that classifies by id, and the
            // look tints are deliberately keyed by kind instead.
            if (items[i] == hairId) continue;
            WeaponTint t;
            if (scope.Lookup(items[i], t) && !t.IsIdentity())
                SwapInTintFor(items[i], sAction.GetBSTR(), t, Layer::Body, weaponTypeCode);
            // An item's effect layers carry their OWN tint under the biased key, so a
            // glow can be recoloured independently of the item it hangs off -- which is
            // the whole point of the window's Effects tab.
            if (scope.Lookup(EffectTintKeyFor(items[i]), t) && !t.IsIdentity())
                SwapInTintFor(items[i], sAction.GetBSTR(), t, Layer::Effects, weaponTypeCode);
        }
        WeaponTint tLook;
        if (hairId > 0 && scope.Lookup(kTintKey_Hair, tLook) && !tLook.IsIdentity())
            SwapInHairTint(hairId, sAction.GetBSTR(), tLook);
        // Face frames normally arrive through BuildFaceLayer, but the v83 client also
        // resolves them while preparing an avatar's action layers on some construction
        // paths (notably the stand preview used by Coloring Prism).  Keep the iris
        // canvases swapped for this whole build too.  The scoped SwapGuard restores the
        // shared WZ nodes before any other avatar can observe them.
        if (faceId > 0 && scope.Lookup(kTintKey_Face, tLook) && !tLook.IsIdentity())
            SwapInFaceTint(faceId, tLook);
        // Skin last: its canvases are the body the others sit on top of, and the swap
        // list is restored in reverse, so this keeps the unwind order the mirror of the
        // build order. It is inside the timed region: it was the one pass the log never
        // measured, and it was the one that cost seconds.
        if (skinId >= 0 && scope.Lookup(kTintKey_Skin, tLook) && !tLook.IsIdentity())
            SwapInSkinTint(skinId, sAction.GetBSTR(), tLook);
        QueryPerformanceCounter(&tSwap1);
        // Builds that cloned nothing are logged too, rate limited: those are the PURE WALK
        // and are the most informative line here. If a build that clones nothing still costs
        // double-digit milliseconds, the walk is the whole cost and the pixel work is noise.
        static int s_quietBuilds = 0;
        const bool interesting = (g_cost.clones > 0) || ((++s_quietBuilds % 60) == 0);
        if (interesting && tFreq.QuadPart) {
            const double freq  = static_cast<double>(tFreq.QuadPart);
            const double total = 1000.0 * static_cast<double>(tSwap1.QuadPart - tSwap0.QuadPart) / freq;
            const double clone = 1000.0 * static_cast<double>(g_cost.cloneTicks) / freq;
            LogMessage("weapontint: swap cost: %d swaps, %d clones (%d bulk / %d per-run), "
                       "%d px | total %.2f ms = walk %.2f + clone %.2f",
                       g_cost.swaps, g_cost.clones, g_cost.bulk, g_cost.perRun, g_cost.pixels,
                       total, total - clone, clone);
        }
        PrepareActionLayer_Orig(pThis, nActionSpeed, nWalkSpeed, bKeyDown);
    }
}

// =====================================================
// THE FACE HOOK
// =====================================================
// The eye tint used to do nothing, and this is why: the face is NOT built inside
// CAvatar::PrepareActionLayer. It has its own builder, 0x00453696, reached from
// CAvatar::SetEmotion at 0x00451D82 -- byte-verified by the two arguments it forwards,
//     00453704  push dword ptr [edi + 0x15]     ; AvatarLook.nFace  (look sits at +4)
//     00453707  push dword ptr [edi + 0x11]     ; AvatarLook.nSkin
//     0045370A  call 0x407a36
// so the swap window around PrepareActionLayer had already closed (or never opened) by
// the time anything read a face canvas. The tinted clones were built and thrown away.
//
// It is a plain __thiscall void(int) (`ret 4` at 0x00453A26) whose ecx is the CAvatar.
// Hooking it is safe as long as nothing else in your DLL does: it is one of the five
// addresses AttachWeaponTintMod claims (see the file header), and the same one-owner
// rule applies to it as to the other four.
//
// This also gets the refresh for free. The builder runs on every emotion change and on
// every blink (CAvatar::RegisterNextBlink @0x00453AA2 schedules them), so a face tint
// appears on its own within a second or two even without an explicit repaint -- and
// WeaponTint_RefreshLocalAvatar calls straight into this hook to make it immediate.
using t_BuildFaceLayer = void(__thiscall*)(void*, int);
auto BuildFaceLayer_Orig =
    reinterpret_cast<t_BuildFaceLayer>(kAddr_CAvatar_BuildFaceLayer);

// The face and eye ACCESSORY slots, read off this avatar's own look.
//
// SEH leaf: raw reads through a pointer that may not be an avatar at all.
bool SehFaceAccessoryIds(CAvatar* pAvatar, int& faceAcc, int& eyeAcc) {
    faceAcc = 0;
    eyeAcc = 0;
    __try {
        faceAcc = pAvatar->m_avatarLook.anHairEquip[2];   // face accessory
        eyeAcc  = pAvatar->m_avatarLook.anHairEquip[3];   // eye accessory
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void __fastcall BuildFaceLayer_Hook(void* pThis, void* /*edx*/, int nDuration) {
    auto* pAvatar = reinterpret_cast<CAvatar*>(pThis);
    const TintScope scope = ScopeForAvatar(pAvatar);
    if (scope.any) {
        int hairId = 0, faceId = 0;
        SehLookIdsOf(pAvatar, hairId, faceId);

        // FACE AND EYE ACCESSORIES ARE BUILT HERE, NOT IN PrepareActionLayer.
        //
        // Their art is resolved inside this call and nowhere else, so the swap window the
        // action build opens has already closed by the time anything reads them -- which is
        // why they rendered vanilla no matter what the Equip tab did to them. Emotion-keyed
        // rather than action-keyed, so the plan covers every expression in one pass.
        //
        // The guard is scoped to this whole call, including BuildFaceLayer_Orig below.
        SwapGuard accessoryGuard;
        int faceAcc = 0, eyeAcc = 0;
        if (SehFaceAccessoryIds(pAvatar, faceAcc, eyeAcc)) {
            for (int accId : { faceAcc, eyeAcc }) {
                if (accId <= 0) continue;
                WeaponTint at;
                if (scope.Lookup(accId, at) && !at.IsIdentity()) {
                    SwapInTintFor(accId, nullptr, at, Layer::Body, -1);
                }
                if (scope.Lookup(EffectTintKeyFor(accId), at) && !at.IsIdentity()) {
                    SwapInTintFor(accId, nullptr, at, Layer::Effects, -1);
                }
            }
        }

        WeaponTint t;
        if (faceId > 0 && scope.Lookup(kTintKey_Face, t) && !t.IsIdentity()) {
            LOG_ONCE_PER_ID(faceId,
                "weapontint: BuildFaceLayer sees tinted face %d (h=%d c=%d v=%d)",
                faceId, t.hue, static_cast<int>(t.chroma), static_cast<int>(t.bright));
            const int nativeFace = NativeFaceForTint(faceId, t);
            FaceIdGuard faceIdGuard(pAvatar, nativeFace);
            BuildFaceLayer_Orig(pThis, nDuration);
            return;
        }
    }
    BuildFaceLayer_Orig(pThis, nDuration);
}

// =====================================================
// NETWORKING
// =====================================================
void Send(const COutPacket& o) {
    void* sock = *reinterpret_cast<void**>(kAddr_ClientSocket_Instance);
    if (!sock) return;
    reinterpret_cast<void(__thiscall*)(void*, const COutPacket&)>(kAddr_ClientSocket_Send)(sock, o);
}

// =====================================================
// ITEM-EFFECT LAYERS
// =====================================================
// A cape aura is not part of the avatar. An effect-layer renderer builds one
// IWzGr2DLayer per equipped item out of Effect/ItemEff.img and runs it with GA_REPEAT
// so it keeps its own Gr2D clock, which is the whole reason it is not an action layer: an
// avatar part is advanced by CAvatar::Update on the BODY's frame index, so its period
// would be forced to equal the current action's and a 12-frame aura would crawl when
// idle and race when walking.
//
// The consequence for tinting is that PrepareActionLayer never sees this art, so the
// hook above cannot reach it. This is the same swap-and-restore opened around that
// separate build, and it uses the item's EFFECT key: a cape's aura and a weapon's glow
// are one feature, one tab and one pair of database columns.
//
// NOT REENTRANT, and it does not need to be: the caller wraps one layer load, on the
// main thread, and such a renderer rebuilds only when the item, the action or the
// facing actually changed.
std::vector<Swap> g_itemEffSwaps;

bool BeginItemEffSwap(CAvatar* pAvatar, int itemId) {
    SwapOutList(g_itemEffSwaps);            // a previous build that never got closed
    if (!pAvatar || itemId <= 0) return false;
    const TintScope scope = ScopeForAvatar(pAvatar);
    if (!scope.any) return false;
    WeaponTint t;
    if (!scope.Lookup(EffectTintKeyFor(itemId), t) || t.IsIdentity()) return false;
    try {
        // THE WHOLE `effect` SUBTREE, not the one posture about to be loaded. 1013 of
        // the posture nodes in this img are UOLs into a sibling, so resolving the
        // posture path can land on an indirection with nothing under it and tint
        // nothing at all. Walking from `effect` down has no such hole and covers every
        // posture at once, which also means a turn or an action change reuses the same
        // swap. It is affordable precisely because these are small: a median of 24
        // canvases per entry and 121 at the very worst, against the few hundred an
        // item's own img would cost, and GetTinted caches every clone after the first.
        wchar_t path[64];
        _snwprintf_s(path, _countof(path), _TRUNCATE, L"Effect/ItemEff.img/%d/effect", itemId);
        IWzPropertyPtr pNode = get_rm()->GetObjectA(path).GetUnknown();
        if (!pNode) return false;
        // Everything below here IS effect art, so this is a Body walk: there is no
        // second half to keep apart, unlike an item's own img.
        SwapSinkTo sink(g_itemEffSwaps);
        SwapSubtree(pNode, t, 0, Layer::Body);
    } catch (...) {
        // Whatever landed in g_itemEffSwaps is still undone by the End call.
    }
    return !g_itemEffSwaps.empty();
}

} // namespace

// =====================================================
// PUBLIC API
// =====================================================
// The prism window builds the preview's cash-effect layer itself, so it needs the same
// swap the two client Detours use. Same contract: always call End if Begin was called.
bool WeaponTint_BeginCashEffectSwap(void* pAvatar, int itemId) {
    return BeginCashEffectSwap(reinterpret_cast<CAvatar*>(pAvatar), itemId);
}

void WeaponTint_EndCashEffectSwap() {
    SwapOutList(g_cashEffSwaps);
}

bool WeaponTint_BeginItemEffSwap(void* pAvatar, int itemId) {
    return BeginItemEffSwap(reinterpret_cast<CAvatar*>(pAvatar), itemId);
}

void WeaponTint_EndItemEffSwap() {
    SwapOutList(g_itemEffSwaps);
}

uint32_t WeaponTint_ItemEffTintKey(void* pAvatar, int itemId) {
    if (!pAvatar || itemId <= 0) return 0;
    const TintScope scope = ScopeForAvatar(reinterpret_cast<CAvatar*>(pAvatar));
    if (!scope.any) return 0;
    WeaponTint t;
    if (!scope.Lookup(EffectTintKeyFor(itemId), t)) return 0;
    return t.Key();
}

// The base weapon an arbitrary avatar is wearing, for callers that have one in hand rather
// than the local user. Slot 11 is the NORMAL weapon even when a cash sticker is drawn over it.
// Zero a canvas to fully transparent, and report whether it worked.
//
// THE ONLY THING THAT ERASES. raw_DrawRectangle and CopyEx(CA_OVERWRITE) with a transparent
// source both return S_OK and leave every pixel untouched, so a window with a region it does not
// repaint in opaque art accumulates rather than clears. Writing the raw locked surface works.
//
// Reuses the bulk write and therefore its guards, the tiled one above all: a canvas past the
// device tile size hands back only its FIRST TILE from get_rawCanvas(0, 0), and zeroing w*h
// pixels through that runs off the end of the tile.
// Zero ONE TILE, `cols` by `rows`, against that tile's own pitch.
bool ClearTileLocked(IWzCanvas* pCanvas, int tx, int ty, int cols, int rows) {
    IWzRawCanvas* raw = nullptr;
    __try {
        if (FAILED(pCanvas->get_rawCanvas(tx, ty, &raw))) raw = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        raw = nullptr;
    }
    if (!raw) return false;

    int pitch = 0;
    uint8_t* base = nullptr;
    __try {
        VARIANT vAddr;
        VariantInit(&vAddr);
        if (SUCCEEDED(raw->raw__LockAddress(&pitch, &vAddr))) {
            const int base_vt = V_VT(&vAddr) & VT_TYPEMASK;
            if (base_vt == VT_I4 || base_vt == VT_UI4
                || base_vt == VT_INT || base_vt == VT_UINT) {
                base = (V_VT(&vAddr) & VT_BYREF)
                     ? reinterpret_cast<uint8_t*>(V_BYREF(&vAddr))
                     : reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(V_I4(&vAddr)));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        base = nullptr;
    }

    bool ok = false;
    if (base && pitch >= cols * 4) {
        __try {
            for (int y = 0; y < rows; ++y) {
                memset(base + static_cast<size_t>(y) * pitch, 0,
                       static_cast<size_t>(cols) * 4);
            }
            ok = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (base) {
        // THE DIRTY RECT IS THE POINT OF THIS CALL. IWzRawCanvas::_UnlockAddress takes
        // `RECT* prc`; with NULL the pixels are zeroed but the renderer keeps drawing its
        // converted copy of the tile, which is how the last frame of a skill effect stayed on
        // screen in the window margin after the cast ended (Brandish's blade tip).
        RECT rcDirty = { 0, 0, cols, rows };
        __try { raw->raw__UnlockAddress(&rcDirty); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    __try { raw->Release(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ok;
}

bool WeaponTint_ClearCanvas(void* pCanvasV, int w, int h) {
    IWzCanvas* pCanvas = reinterpret_cast<IWzCanvas*>(pCanvasV);
    if (!pCanvas || w <= 0 || h <= 0) return false;

    CANVAS_PIXFORMAT fmt = CP_UNKNOWN;
    __try {
        if (FAILED(pCanvas->get_pixelFormat(&fmt))) fmt = CP_UNKNOWN;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fmt = CP_UNKNOWN;
    }
    if (fmt != CP_A8R8G8B8) return false;

    unsigned int cw = 0, ch = 0, tw = 0, th = 0;
    __try {
        if (FAILED(pCanvas->get_width(&cw)))      cw = 0;
        if (FAILED(pCanvas->get_height(&ch)))     ch = 0;
        if (FAILED(pCanvas->get_tileWidth(&tw)))  tw = 0;
        if (FAILED(pCanvas->get_tileHeight(&th))) th = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cw = ch = tw = th = 0;
    }
    if (static_cast<int>(cw) != w || static_cast<int>(ch) != h) return false;
    if (!tw) tw = static_cast<unsigned int>(w);        // untiled: one tile covering everything
    if (!th) th = static_cast<unsigned int>(h);

    const int tW = static_cast<int>(tw), tH = static_cast<int>(th);
    const int tilesX = (w + tW - 1) / tW;
    const int tilesY = (h + tH - 1) / tH;
    const bool byTile = RawCanvasIndexIsTile(pCanvas, tilesX, tilesY);

    bool all = true;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            // The last band and column are SHORT. Zeroing a full tile there would run past
            // the end of the allocation, which is the same mistake that corrupted the heap
            // when clone writes ignored tiling.
            const int rows = (h - ty * tH < tH) ? (h - ty * tH) : tH;
            const int cols = (w - tx * tW < tW) ? (w - tx * tW) : tW;
            if (rows <= 0 || cols <= 0) continue;
            const int ix = byTile ? tx : tx * tW;
            const int iy = byTile ? ty : ty * tH;
            if (!ClearTileLocked(pCanvas, ix, iy, cols, rows)) all = false;
        }
    }
    LOG_ONCE("weapontint: first canvas clear of %dx%d over %dx%d tiles of %dx%d, indexed by "
             "%s: %s", w, h, tilesX, tilesY, tW, tH, byTile ? "tile" : "pixel",
             all ? "ok" : "FAILED");
    return all;
}

// Does any node under this property carry effect art? Depth-bounded the same way the swap
// walks are, because an item img is frames inside actions and the answer is always shallow.
bool HasEffectChild(IWzPropertyPtr pNode, int depth) {
    if (!pNode || depth > 3) return false;
    for (const std::wstring& name : ChildNames(pNode)) {
        if (IsEffectPart(name.c_str())) return true;
        Ztl_variant_t v = pNode->item[name.c_str()];
        IUnknownPtr pUnk = get_unknown(v);
        if (!pUnk) continue;
        // A canvas answers QueryInterface(IWzProperty) too, so descending blindly would walk
        // into origin/z/map. Only a node that is NOT a canvas is worth going into.
        IWzCanvasPtr pCanvas;
        if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pCanvas)) && pCanvas) continue;
        IWzPropertyPtr pSub;
        if (FAILED(pUnk.QueryInterface(__uuidof(IWzProperty), &pSub)) || !pSub) continue;
        if (HasEffectChild(pSub, depth + 1)) return true;
    }
    return false;
}

bool WeaponTint_ItemHasEffectArt(int itemId) {
    if (itemId <= 0) return false;
    // A cash EFFECT item is not asked about here: it IS the glow, has no body sprite, and the
    // window answers that from the id range without a walk.
    static std::map<int, bool> s_cache;
    auto it = s_cache.find(itemId);
    if (it != s_cache.end()) return it->second;

    bool has = false;
    const wchar_t* subdir = CharacterSubdirOf(itemId);
    if (subdir) {
        try {
            wchar_t path[80];
            _snwprintf_s(path, _countof(path), _TRUNCATE,
                         L"Character/%s/%08d.img", subdir, itemId);
            IWzPropertyPtr pImg = get_rm()->GetObjectA(path).GetUnknown();
            has = HasEffectChild(pImg, 0);
        } catch (...) {
        }
    }
    s_cache.emplace(itemId, has);
    return has;
}

namespace {

// Skill/<img>/skill/<id>. `%07d` is a minimum width, so the eight-digit Cygnus and Aran ids
// (Skill/1100.img/skill/11001004) come out whole.
IWzPropertyPtr SkillImgNode(int skillId) {
    if (skillId <= 0) return nullptr;
    wchar_t path[96];
    _snwprintf_s(path, _countof(path), _TRUNCATE,
                 L"Skill/%03d.img/skill/%07d", skillId / 10000, skillId);
    try {
        return get_rm()->GetObjectA(path).GetUnknown();
    } catch (...) {
        return nullptr;
    }
}

// Is there a canvas anywhere under this node (or is it one)? Canvas FIRST, for the reason the
// swap walks give: descending on the property interface first would walk into a canvas's own
// origin / delay children and find nothing. Depth-bounded like the walks: a part is at most
// <layer>/<frame> deep.
bool HasCanvasUnder(IUnknownPtr pUnk, int depth) {
    if (!pUnk || depth > 4) return false;
    IWzCanvasPtr pCanvas;
    if (SUCCEEDED(pUnk.QueryInterface(__uuidof(IWzCanvas), &pCanvas)) && pCanvas) return true;
    IWzPropertyPtr pNode;
    if (FAILED(pUnk.QueryInterface(__uuidof(IWzProperty), &pNode)) || !pNode) return false;
    for (const std::wstring& name : ChildNames(pNode)) {
        Ztl_variant_t v = pNode->item[name.c_str()];
        IUnknownPtr pSub = get_unknown(v);
        if (pSub && HasCanvasUnder(pSub, depth + 1)) return true;
    }
    return false;
}

// What a skill offers the Skills tab, worked out once per skill. A part is listed only when a
// node of it holds real art: a `hit` with nothing but an `onlyOnce` flag under it is no more
// dyeable than `level`, and offering it would take a prism for a colour nothing draws.
struct SkillPartInfo {
    std::vector<WeaponTintSkillChild> children;     // node order
    int  parts[kSkillPartCount] = {};                // ascending
    int  nParts = 0;
    bool has[kSkillPartCount + 1] = {};
};

const SkillPartInfo& SkillPartsOf(int skillId) {
    static std::map<int, SkillPartInfo> s_cache;
    auto it = s_cache.find(skillId);
    if (it != s_cache.end()) return it->second;
    SkillPartInfo info;
    try {
        IWzPropertyPtr pNode = SkillImgNode(skillId);
        if (pNode) {
            for (const std::wstring& name : ChildNames(pNode)) {
                const int part = SkillPartOf(name.c_str());
                if (part == kSkillPart_None) continue;
                if (name.size() >= sizeof(WeaponTintSkillChild::name) / sizeof(wchar_t)) continue;
                Ztl_variant_t v = pNode->item[name.c_str()];
                if (!HasCanvasUnder(get_unknown(v), 0)) continue;
                WeaponTintSkillChild c;
                c.part = part;
                wcsncpy_s(c.name, _countof(c.name), name.c_str(), _TRUNCATE);
                info.children.push_back(c);
                info.has[part] = true;
            }
        }
    } catch (...) {
    }
    for (int p = kSkillPartMin; p <= kSkillPartMax; ++p) {
        if (info.has[p]) info.parts[info.nParts++] = p;
    }
    LOG_ONCE_PER_ID(skillId, "weapontint: skill %d has %d dyeable part(s) over %d node(s)",
                    skillId, info.nParts, static_cast<int>(info.children.size()));
    return s_cache.emplace(skillId, std::move(info)).first->second;
}

} // namespace

int WeaponTint_SkillPartOfChild(const wchar_t* name) {
    return SkillPartOf(name);
}

int WeaponTint_ListSkillParts(int skillId, int partsOut[], int cap) {
    if (skillId <= 0 || !partsOut || cap <= 0) return 0;
    const SkillPartInfo& info = SkillPartsOf(skillId);
    int n = 0;
    for (int i = 0; i < info.nParts && n < cap; ++i) partsOut[n++] = info.parts[i];
    return n;
}

bool WeaponTint_SkillHasPart(int skillId, int part) {
    if (skillId <= 0 || !IsSkillPartValid(part)) return false;
    return SkillPartsOf(skillId).has[part];
}

int WeaponTint_ListSkillPartChildren(int skillId, WeaponTintSkillChild out[], int cap) {
    if (skillId <= 0 || !out || cap <= 0) return 0;
    const SkillPartInfo& info = SkillPartsOf(skillId);
    int n = 0;
    for (const WeaponTintSkillChild& c : info.children) {
        if (n >= cap) break;
        out[n++] = c;
    }
    return n;
}

const wchar_t* WeaponTint_SkillPartLabel(int part) {
    static const wchar_t* const kLabels[kSkillPartCount + 1] = {
        L"",
        L"Effect", L"Effect 0", L"Effect 1", L"Effect 2", L"Effect 3",
        L"Ball", L"Hit", L"Affected", L"Special", L"Prepare",
        L"Keydown", L"Keydown End", L"Repeat", L"Finish", L"Screen", L"Tile", L"Mob",
    };
    return IsSkillPartValid(part) ? kLabels[part] : L"";
}

int WeaponTint_BaseWeaponIdOf(void* pAvatar) {
    if (!pAvatar) return 0;
    return SehBaseWeaponIdOf(reinterpret_cast<CAvatar*>(pAvatar));
}

int WeaponTint_GetLocalCashWeaponId() {
    return CashWeaponOf(LocalAvatar());
}

int WeaponTint_GetLocalHairId() {
    int hair = 0, face = 0;
    SehLookIdsOf(LocalAvatar(), hair, face);
    return hair;
}

int WeaponTint_GetLocalFaceId() {
    int hair = 0, face = 0;
    SehLookIdsOf(LocalAvatar(), hair, face);
    return face;
}

// -1 when there is no local avatar. Skin 0 is a real skin, so 0 cannot mean "none".
int WeaponTint_GetLocalSkinId() {
    int hair = 0, face = 0, skin = -1;
    SehLookIdsOf(LocalAvatar(), hair, face, skin);
    return skin;
}

WeaponTint WeaponTint_GetSavedFor(int itemId) {
    std::lock_guard<std::mutex> lock(g_mtx);
    auto it = g_saved.find(itemId);
    return (it == g_saved.end()) ? WeaponTint{} : it->second;
}

WeaponTint WeaponTint_GetEffectiveFor(int itemId) {
    if (g_previewActive && itemId == g_previewItemId) return g_preview;
    return WeaponTint_GetSavedFor(itemId);
}

bool WeaponTint_IsPreviewActive() { return g_previewActive; }

void WeaponTint_SetPreview(int itemId, const WeaponTint& t, bool active) {
    const WeaponTint before = WeaponTint_GetEffectiveFor(itemId);
    const int beforeId = g_previewItemId;
    g_preview = t;
    g_previewItemId = itemId;
    g_previewActive = active;
    if (beforeId != itemId || WeaponTint_GetEffectiveFor(itemId) != before) {
        // Drop the clones so the WINDOW's avatar rebuilds in the new colour. ONLY the ones
        // made with the outgoing colour: everything else the character wears is unaffected
        // by this slider and its clones are still valid, and re-cloning them was costing a
        // full outfit's worth of work on every drag step.
        //
        // The world avatar is deliberately NOT refreshed: the preview no longer reaches it
        // (see ScopeForAvatar), so repainting it would only cost a layer rebuild per step.
        ClearClonesForTint(before.Key());
    }
}

void WeaponTint_AdoptOptimistic(int itemId, const WeaponTint& t) {
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (t.IsIdentity()) g_saved.erase(itemId);
        else                g_saved[itemId] = t;
        // A skill has one key PER PART. Confirming Ball must not let the next snapshot drop
        // the Effect colour the player confirmed a moment earlier (a cap, an older server,
        // the apply of the other part still in flight), so every saved part key of this
        // skill is protected until that snapshot lands. Restore of THIS key has already
        // erased it, so it is not re-protected.
        if (IsSkillPartKey(itemId)) {
            const int skillId = SkillOfPartKey(itemId);
            for (const auto& kv : g_saved) {
                if (IsSkillPartKey(kv.first) && SkillOfPartKey(kv.first) == skillId)
                    g_protectKeys.insert(kv.first);
            }
        }
    }
    // Confirm is the moment the world character is supposed to change, and now the only
    // one: the live preview never touched it. Repaint here rather than relying on the
    // window's teardown, whose before/after tints are equal by this point and so would
    // detect no change at all.
    ClearClones();
    WeaponTint_RefreshLocalAvatar();
    // ...and the cash effect layer, for exactly the reason the comment above gives, which
    // bites twice as hard here. This function writes the new colour into g_saved BEFORE
    // the server answers, so when the authoritative snapshot arrives it compares EQUAL,
    // g_syncDirty is never set, and the per-frame tick returns before it can rebuild
    // anything. Every other tint survives that because the two lines above repaint them
    // directly; the effect layer has to be replayed here for the same reason.
    ReshowLocalCashEffect();
}

void WeaponTint_BeginForcedScope() {
    ++g_forcedScope;
    g_forcedAvatar = nullptr;
    g_forcedTints.clear();
}

void WeaponTint_BeginForcedScope(void* pAvatar) {
    ++g_forcedScope;
    g_forcedAvatar = reinterpret_cast<CAvatar*>(pAvatar);
    g_forcedTints.clear();
}

void WeaponTint_BeginForcedScope(void* pAvatar, const WeaponTint& hairTint,
                                 const WeaponTint& faceTint) {
    WeaponTint_BeginForcedScope(pAvatar);
    // Keep both keys, including identity values. They must shadow the local
    // character's saved tint when this is a thumbnail render.
    g_forcedTints[kTintKey_Hair] = hairTint;
    g_forcedTints[kTintKey_Face] = faceTint;
}

void WeaponTint_EndForcedScope() {
    if (g_forcedScope <= 0) return;
    if (--g_forcedScope > 0) return;
    g_forcedScope = 0;
    g_forcedAvatar = nullptr;
    g_forcedTints.clear();
}

void WeaponTint_BindPreviewAvatar(void* pAvatar) {
    g_prismPreviewAvatar = reinterpret_cast<CAvatar*>(pAvatar);
}

void WeaponTint_UnbindPreviewAvatar(void* pAvatar) {
    if (g_prismPreviewAvatar == reinterpret_cast<CAvatar*>(pAvatar)) {
        SwapOutList(g_previewFaceSwaps);
        g_prismPreviewAvatar = nullptr;
    }
}

void WeaponTint_InstallPreviewFaceTint(void* pAvatar) {
    auto* avatar = reinterpret_cast<CAvatar*>(pAvatar);
    if (!avatar || avatar != g_prismPreviewAvatar) return;
    int hairId = 0, faceId = 0;
    if (!SehLookIdsOf(avatar, hairId, faceId) || faceId <= 0) return;
    const TintScope scope = ScopeForAvatar(avatar);
    WeaponTint tint;
    if (!scope.Lookup(kTintKey_Face, tint) || tint.IsIdentity()) return;
    // A new preview has no old retained nodes, but restore defensively in case a
    // failed build reused this allocation before its normal teardown ran.
    SwapOutList(g_previewFaceSwaps);
    SwapInFaceTintInto(faceId, tint, g_previewFaceSwaps);
}

void WeaponTint_RefreshLocalAvatar() {
    CAvatar* pAvatar = LocalAvatar();
    if (!pAvatar) return;
    SehReloadActionLayer(pAvatar);
    // +4E0 is the client's non-owning face-build sentinel.  Its constructor sets it
    // to null and BuildFaceLayer uses only null/non-null to decide whether to make a
    // new face layer.  Clear that sentinel before an explicit cosmetic refresh so a
    // confirmed eye tint does not wait for relogging to become visible.
    __try { *reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(pAvatar) + 0x4E0) = nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    // The face sits outside the action layers, so the reload above does not touch it. Go
    // through the HOOK rather than the original, so the swap is applied; -1 is the duration
    // CAvatar::Init itself passes down this path.
    BuildFaceLayer_Hook(pAvatar, nullptr, -1);
}

WeaponTintResult WeaponTint_TakeLastResult() {
    std::lock_guard<std::mutex> lock(g_mtx);
    WeaponTintResult r = g_lastResult;
    g_lastResult = kTintResult_None;
    return r;
}

void WeaponTint_RequestSnapshot() {
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_RequestSnapshot);
    Send(o);
}

void WeaponTint_SendApply(const WeaponTintTarget& target, const WeaponTint& t, int prismPos, int layer) {
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_Apply);
    o.Encode1(static_cast<unsigned char>(target.invType));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(target.invPos)));
    o.Encode4(static_cast<unsigned int>(target.itemId));
    o.Encode2(static_cast<unsigned short>(t.hue));
    o.Encode1(static_cast<unsigned char>(t.chroma));
    o.Encode1(static_cast<unsigned char>(t.bright));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(prismPos)));
    o.Encode1(static_cast<unsigned char>(layer));
    Send(o);
}

void WeaponTint_SendRestore(const WeaponTintTarget& target, int prismPos, int layer) {
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_Restore);
    o.Encode1(static_cast<unsigned char>(target.invType));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(target.invPos)));
    o.Encode4(static_cast<unsigned int>(target.itemId));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(prismPos)));
    o.Encode1(static_cast<unsigned char>(layer));
    Send(o);
}

// Actions 5 / 6: skillId(4) hue(2) chroma(1) bright(1) prismPos(2) part(1), and
// skillId(4) prismPos(2) part(1). The PART byte is required and is a kSkillPart_* id; the
// server stores the colour under SkillPartKeyFor(skillId, part), which is the key its
// snapshot sends back and the key BeginSkillSwap looks up. A part id outside 1..17 is a
// window bug, so it is refused here rather than sent for the server to refuse.
void WeaponTint_SendApplySkill(int skillId, const WeaponTint& t, int prismPos, int part) {
    if (skillId <= 0 || !IsSkillPartValid(part)) return;
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_ApplySkill);
    o.Encode4(static_cast<unsigned int>(skillId));
    o.Encode2(static_cast<unsigned short>(t.hue));
    o.Encode1(static_cast<unsigned char>(t.chroma));
    o.Encode1(static_cast<unsigned char>(t.bright));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(prismPos)));
    o.Encode1(static_cast<unsigned char>(part));
    Send(o);
}
void WeaponTint_SendRestoreSkill(int skillId, int prismPos, int part) {
    if (skillId <= 0 || !IsSkillPartValid(part)) return;
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_RestoreSkill);
    o.Encode4(static_cast<unsigned int>(skillId));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(prismPos)));
    o.Encode1(static_cast<unsigned char>(part));
    Send(o);
}
// No target: the server reads the character's OWN hair/face, so there is nothing here
// for it to re-verify against and nothing the client could lie about.
void WeaponTint_SendApplyLook(int kind, const WeaponTint& t, int prismPos) {
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_ApplyLook);
    o.Encode1(static_cast<unsigned char>(kind));
    o.Encode2(static_cast<unsigned short>(t.hue));
    o.Encode1(static_cast<unsigned char>(t.chroma));
    o.Encode1(static_cast<unsigned char>(t.bright));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(prismPos)));
    Send(o);
}

void WeaponTint_SendRestoreLook(int kind, int prismPos) {
    COutPacket o(kWeaponTintActionOpcode);
    o.Encode1(kAction_RestoreLook);
    o.Encode1(static_cast<unsigned char>(kind));
    o.Encode2(static_cast<unsigned short>(static_cast<short>(prismPos)));
    Send(o);
}

// Runs on the RECEIVE thread. Everything it writes is under g_mtx; the render
// side picks the change up on its next frame.
void WeaponTint_HandleSync(CInPacket* p) {
    if (!p) return;
    // The opcode sits at the packet's CURRENT offset, not necessarily 0.
    p->SetOffset(p->GetOffset() + 2);
    if (!p->CanRead(1)) return;
    const uint8_t subtype = p->Decode<uint8_t>();

    // Shared entry reader: itemId(4) hue(2) chroma(1) bright(1).
    auto readEntry = [&](TintMap& into) -> bool {
        if (!p->CanRead(8)) return false;
        const int itemId = static_cast<int>(p->Decode<uint32_t>());
        WeaponTint t;
        t.hue    = static_cast<short>(p->Decode<uint16_t>());
        t.chroma = static_cast<signed char>(p->Decode<uint8_t>());
        t.bright = static_cast<signed char>(p->Decode<uint8_t>());
        // BOTH SIGNS ARE LEGAL. Positive rotates by that many degrees; negative is an
        // absolute target encoded as -(degrees + 1). Rejecting everything below zero here
        // stripped the hue out of every absolute tint that came down the wire.
        if (!WeaponTint::IsHueValid(t.hue)) t.hue = 0;
        if (itemId > 0 && !t.IsIdentity()) into[itemId] = t;
        return true;
    };

    if (subtype == kResp_Snapshot) {
        // count + one entry per DYED item the local player owns.
        if (!p->CanRead(1)) return;
        const int count = p->Decode<uint8_t>();
        TintMap parsed;
        for (int i = 0; i < count; ++i) {
            if (!readEntry(parsed)) return;      // truncated: keep what we had
        }
        std::lock_guard<std::mutex> lock(g_mtx);
        for (int key : g_protectKeys) {
            if (parsed.find(key) != parsed.end()) continue;
            auto it = g_saved.find(key);
            if (it != g_saved.end() && !it->second.IsIdentity()) parsed[key] = it->second;
        }
        g_protectKeys.clear();
        if (parsed != g_saved) g_syncDirty = true;
        g_saved.swap(parsed);
        return;
    }

    if (subtype == kResp_RemoteTable) {
        // playerCount, then per player: name(u16 len + bytes), itemCount, entries.
        if (!p->CanRead(1)) return;
        const int players = p->Decode<uint8_t>();
        std::unordered_map<std::string, TintMap> parsed;
        for (int i = 0; i < players; ++i) {
            if (!p->CanRead(2)) return;
            const int len = p->Decode<uint16_t>();
            if (len < 1 || len > 13 || !p->CanRead(static_cast<size_t>(len) + 1)) return;
            std::string name(reinterpret_cast<const char*>(p->CurrentPublic()), len);
            p->SetOffset(p->GetOffset() + len);
            const int items = p->Decode<uint8_t>();
            TintMap m;
            for (int k = 0; k < items; ++k) {
                if (!readEntry(m)) return;
            }
            if (!m.empty()) parsed.emplace(std::move(name), std::move(m));
        }
        std::lock_guard<std::mutex> lock(g_mtx);
        // Repaint anyone whose tints actually moved, in EITHER direction -- a name
        // that dropped out of the table has been restored to vanilla and needs the
        // rebuild just as much as one that gained a colour.
        for (const auto& kv : parsed) {
            auto old = g_remoteTints.find(kv.first);
            if (old == g_remoteTints.end() || old->second != kv.second) {
                g_remoteDirty.push_back(kv.first);
            }
        }
        for (const auto& kv : g_remoteTints) {
            if (parsed.find(kv.first) == parsed.end()) g_remoteDirty.push_back(kv.first);
        }
        g_remoteTints.swap(parsed);
        return;
    }

    if (subtype == kResp_Result) {
        if (!p->CanRead(1)) return;
        const uint8_t code = p->Decode<uint8_t>();
        std::lock_guard<std::mutex> lock(g_mtx);
        g_lastResult = (code <= kTintResult_Failed)
                     ? static_cast<WeaponTintResult>(code)
                     : kTintResult_Failed;
        return;
    }
}

// Main thread, once per frame (the host's CWvsApp::CallUpdate hook). This is what
// makes a SERVER-driven tint change
// visible: an apply and a restore both land as a snapshot on the receive thread,
// which can only raise a flag. Without this the weapon kept its old colour until
// something else happened to rebuild its layers -- unequipping and re-equipping it,
// for instance.
// Defined with the skill swap further down; declared here because the tick is what puts a
// held swap back and it is compiled first.
void ExpireSkillSwap();

void WeaponTint_Tick() {
    // A skill swap outlives its cast on purpose, so that the mob hit animation is built from
    // tinted art. This is what puts the tree back.
    ExpireSkillSwap();

    bool dirty = false;
    std::vector<std::string> remote;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        dirty = g_syncDirty;
        g_syncDirty = false;
        remote.swap(g_remoteDirty);
    }

    // --- other players -------------------------------------------------------
    // Their clones live in the same cache as ours, so any change there invalidates
    // it wholesale. Each affected avatar is then reloaded directly; a name we have
    // never rendered simply has no entry, and picks its tint up naturally the first
    // time the renderer asks for it.
    if (!remote.empty()) {
        ClearClones();
        for (const std::string& name : remote) {
            CAvatar* pAvatar = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mtx);
                auto it = g_avatarByName.find(name);
                if (it == g_avatarByName.end()) continue;
                pAvatar = it->second;
            }
            // The pointer may be stale -- that player could have left the map and
            // the CUser been freed. Re-read the name off it and only touch it if it
            // still says what we recorded.
            char check[16];
            if (!NameOfAvatar(pAvatar, check, sizeof(check)) || name != check) {
                std::lock_guard<std::mutex> lock(g_mtx);
                g_avatarByName.erase(name);
                continue;
            }
            SehReloadActionLayer(pAvatar);
            // ...and their face, which is a separate layer the reload above does not
            // touch. Without this a remote player's eye colour would not appear until
            // their next blink. Safe to call here: the lock is already released, and
            // BuildFaceLayer_Hook takes it itself.
            BuildFaceLayer_Hook(pAvatar, nullptr, -1);
        }
    }

    // --- the local player ----------------------------------------------------
    if (!dirty) return;
    // While the prism window is open its sliders own the render; adopting the
    // server's value now would yank the preview out from under the player mid-drag.
    // The window drops the preview when it closes, and that path repaints itself.
    if (g_previewActive) return;
    ClearClones();
    WeaponTint_RefreshLocalAvatar();
    // A cash effect is its own Gr2D layer, built only when the client is told to show one,
    // so a recolour leaves it holding the old canvases. Replay the last build.
    ReshowLocalCashEffect();
}


// =====================================================
// SKILL EFFECTS
// =====================================================
// CUser::ShowSkillEffect. __thiscall(CUser* caster, SKILLENTRY* pSkill, int, int, int),
// ret 0x14. Non-virtual, 24 direct callers spanning the local cast paths, the remote attack
// packet path and CUser::OnUserEffect -- so one hook covers every way a skill effect reaches
// the screen, for every character in the map.
//
// THE CASTER IS `this`, which is the whole reason this address is the right one. The layer
// it builds is handed to a global animation list at [0x00BEBF6C]+0x30 with the local
// reference dropped, so there is no per-user layer to find afterwards and no way to colour
// one later; it has to happen inside this call.
//
// The art is reachable because the startup load stores UOL STRINGS on the SKILLENTRY rather
// than resolved canvases, and the layer factory re-resolves them through GetObjectA on every
// call. Each dyeable PART of the skill (effect, effect0..3, ball, hit, affected, special,
// prepare, keydown, keydownend, repeat, finish, screen, tile, mob -- see weapontint.h) is
// swapped in its own colour in one pass; nothing else under the node is touched.
//
// FIVE STACK ARGUMENTS, not four. The function ends `ret 0x14` at 0x009362EC, so it pops 20
// bytes; a four-argument declaration pops 16 and leaves the stack four bytes out on every
// cast, which crashes shortly afterwards in whatever ran next rather than here. Read the
// epilogue, do not infer the arity from the call sites.
constexpr uintptr_t kAddr_CUser_ShowSkillEffect = 0x00933990;
using t_ShowSkillEffect = void(__thiscall*)(void*, void*, int, int, int, int);
auto ShowSkillEffect_Orig =
    reinterpret_cast<t_ShowSkillEffect>(kAddr_CUser_ShowSkillEffect);

// ONE ENTRY PER SKILL, not one slot for all of them.
//
// A skill's art has to stay swapped in long after its cast returns, because projectiles and mob
// hit animations resolve their sprites on later ticks. With a single shared list, the next skill
// to be cast -- Final Attack on a bow procs within half a second, every time -- tore out whatever
// was already there, and everything still waiting to resolve fell back to the original art.
struct HeldSkillSwap {
    int               skillId = 0;
    // Every (part, colour) this swap installed, ascending by part. Compared as a whole: the
    // same skill at the same colours reuses the held swap; any part changing colour, or
    // gaining or losing one, rebuilds it.
    std::vector<SkillPartTint> tints;
    std::vector<Swap> swaps;
    DWORD             expiry = 0;      // GetTickCount deadline; 0 means "not held"
};
std::vector<HeldSkillSwap> g_heldSkillSwaps;

// The preview window drives its own swap through the same walk, and it is not keyed by skill
// because only one preview exists at a time.
std::vector<Swap> g_skillSwaps;

// The skill id is the first member of SKILLENTRY, which CUserPool and the effect dispatcher
// both read the same way.
int SehSkillIdOf(void* pSkillEntry) {
    int id = 0;
    __try {
        id = *reinterpret_cast<int*>(pSkillEntry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return id;
}

// Swap the dyeable parts of `Skill/<job>.img/skill/<id>` for one build.
// The skill whose cast is currently in flight, so a bullet queued during it can name the swap
// it belongs to. The bullet call carries no skill id of its own.
int g_lastSkillSwapId = 0;

void HoldSkillSwap(int skillId, DWORD ms);
bool IsSkillSwapHeld(int skillId);

bool SameSkillTints(const std::vector<SkillPartTint>& a, const std::vector<SkillPartTint>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].part != b[i].part || a[i].tint != b[i].tint) return false;
    }
    return true;
}

// The non-identity colour of every part this skill actually has, for this avatar. Only the
// skill's own parts are looked up (at most seventeen lookups, usually two or three), so a cast
// of an undyed skill costs one cached list and nothing else.
std::vector<SkillPartTint> SkillTintsFor(const TintScope& scope, int skillId) {
    std::vector<SkillPartTint> out;
    if (!scope.any || skillId <= 0) return out;
    int parts[kSkillPartCount];
    const int n = WeaponTint_ListSkillParts(skillId, parts, kSkillPartCount);
    for (int i = 0; i < n; ++i) {
        WeaponTint t;
        if (scope.Lookup(SkillPartKeyFor(skillId, parts[i]), t) && !t.IsIdentity())
            out.push_back(SkillPartTint{ parts[i], t });
    }
    return out;
}

// Does this cast need the LONG hold? Projectiles resolve their `ball` sprite on a later tick,
// and `hit` / `mob` art is built when the damage comes back -- both well after the 400 ms
// that covers a caster-side effect. Set by BeginSkillSwap for the hook that holds it.
bool g_skillSwapWantsLongHold = false;

bool BeginSkillSwap(CAvatar* pAvatar, int skillId) {
    g_skillSwapWantsLongHold = false;
    if (skillId <= 0 || !pAvatar) return false;
    const TintScope scope = ScopeForAvatar(pAvatar);
    std::vector<SkillPartTint> tints = SkillTintsFor(scope, skillId);
    if (tints.empty()) return false;
    for (const SkillPartTint& pt : tints) {
        if (pt.part == kSkillPart_Hit || pt.part == kSkillPart_Mob) g_skillSwapWantsLongHold = true;
    }
    if (WeaponTint_SkillHasPart(skillId, kSkillPart_Ball)) g_skillSwapWantsLongHold = true;

    // ALREADY HELD for this skill at these colours: reuse it. Rebuilding would mean
    // restore-then-reinstall, and every one of those is an instant where the tree holds the
    // ORIGINAL art -- a projectile resolving across it takes the original for good.
    for (HeldSkillSwap& h : g_heldSkillSwaps) {
        if (h.skillId == skillId && !h.swaps.empty()) {
            if (SameSkillTints(h.tints, tints)) return true;
            SwapOutList(h.swaps);            // same skill, new colours: this one has to go
            break;
        }
    }
    g_heldSkillSwaps.erase(
            std::remove_if(g_heldSkillSwaps.begin(), g_heldSkillSwaps.end(),
                           [](const HeldSkillSwap& h) { return h.swaps.empty(); }),
            g_heldSkillSwaps.end());

    g_heldSkillSwaps.emplace_back();
    HeldSkillSwap& held = g_heldSkillSwaps.back();
    held.skillId = skillId;
    held.tints   = tints;
    try {
        IWzPropertyPtr pNode = SkillImgNode(skillId);
        if (!pNode) {
            g_heldSkillSwaps.pop_back();
            return false;
        }
        SwapSinkTo sink(held.swaps);
        // PER PART, each in its own colour. Only the direct children that are dyeable parts
        // are touched, so the icon and everything else in the node keep their pixels.
        SwapSkillParts(pNode, tints);

        // WHAT DID WE ACTUALLY TINT? Bounded to the first few casts. Count this cast's
        // held list, not the preview's g_skillSwaps (that one is empty on a world cast).
        static int s_left = 3;
        if (s_left > 0) {
            --s_left;
            LogMessage("weapontint: skill %d, %u tinted part(s), swapped %u canvases", skillId,
                       static_cast<unsigned>(tints.size()),
                       static_cast<unsigned>(held.swaps.size()));
            for (const std::wstring& child : ChildNames(pNode)) {
                const int part = SkillPartOf(child.c_str());
                if (part == kSkillPart_None) continue;
                Ztl_variant_t cv = pNode->item[child.c_str()];
                IUnknownPtr cu = get_unknown(cv);
                if (!cu) continue;
                IWzPropertyPtr cp;
                if (FAILED(cu.QueryInterface(__uuidof(IWzProperty), &cp)) || !cp) continue;
                int mine = 0;
                for (const Swap& s : held.swaps) {
                    if (s.parent.GetInterfacePtr() == cp.GetInterfacePtr()) ++mine;
                }
                LogMessage("weapontint:   %-16S part=%-2d swapped=%d (direct children)",
                           child.c_str(), part, mine);
            }
        }
    } catch (...) {
    }
    if (held.swaps.empty()) {
        g_heldSkillSwaps.pop_back();
        return false;
    }
    return true;
}

// Give the skill's held swap a deadline, pushing it out rather than pulling it in.
void HoldSkillSwap(int skillId, DWORD ms) {
    const DWORD want = GetTickCount() + ms;
    for (HeldSkillSwap& h : g_heldSkillSwaps) {
        if (h.skillId != skillId) continue;
        if (!h.expiry || static_cast<int>(want - h.expiry) > 0) h.expiry = want ? want : 1;
        return;
    }
}

// Is anything held for this skill right now? The bullet extension must not prolong a swap that
// was never installed.
bool IsSkillSwapHeld(int skillId) {
    for (const HeldSkillSwap& h : g_heldSkillSwaps) {
        if (h.skillId == skillId && !h.swaps.empty()) return true;
    }
    return false;
}

// HOW LONG THE SWAP STAYS IN THE TREE AFTER A CAST.
//
// Long enough for the damage to come back and the mob hit animation to be built from the tinted
// art, short enough that two players casting the same skill rarely overlap inside it. The tree
// is global, so during the hold another caster of that skill would render in this one's colour;
// that is the cost of not having a hook at the hit builder, and it is cosmetic and transient.
constexpr DWORD kSkillSwapHoldMs = 400;
// Projectiles are only QUEUED in ShowSkillEffect. CreateBullet resolves the ball
// sprite on a later ~30ms tick, often after 400ms has already restored the tree. A tinted
// `hit` or `mob` part takes the same hold: that art is built when the damage packet comes
// back, which routinely lands after 400ms too.
constexpr DWORD kSkillBallHoldMs = 3500;

// Restore held swaps whose window is up, each on its own deadline. Called from the per-frame
// tick. Independent per skill, so one skill's volley cannot cut another's short.
void ExpireSkillSwap() {
    const DWORD now = GetTickCount();
    for (HeldSkillSwap& h : g_heldSkillSwaps) {
        if (!h.expiry || static_cast<int>(now - h.expiry) < 0) continue;
        SwapOutList(h.swaps);
        h.expiry = 0;
    }
    g_heldSkillSwaps.erase(
            std::remove_if(g_heldSkillSwaps.begin(), g_heldSkillSwaps.end(),
                           [](const HeldSkillSwap& h) { return h.swaps.empty(); }),
            g_heldSkillSwaps.end());
}

void __fastcall ShowSkillEffect_Hook(void* pThis, void* /*edx*/, void* pSkillEntry,
                                     int a2, int a3, int a4, int a5) {
    // RUNS FOR REMOTE CASTS TOO, verified from a live log rather than assumed, which is what
    // makes another player's dyed skill visible: ScopeForAvatar resolves a remote avatar by
    // name out of the map table.
    CAvatar* pAvatar = AvatarOfUser(pThis);
    // HELD, not scoped. The mob hit animation is built when the damage lands, which is after
    // this call returns, so restoring on the way out left it with the original art. The expiry
    // is set BEFORE the original runs so an exception out of it cannot strand the swap.
    const int skillId = SehSkillIdOf(pSkillEntry);
    if (BeginSkillSwap(pAvatar, skillId)) {
        // Held against THIS skill. Nothing here touches any other skill's swap, which is the
        // whole point: Final Attack firing half a second into a volley used to evict it.
        // The long hold when a projectile or a tinted hit / mob part still has to resolve.
        const DWORD hold = g_skillSwapWantsLongHold ? kSkillBallHoldMs : kSkillSwapHoldMs;
        HoldSkillSwap(skillId, hold);
        g_lastSkillSwapId = skillId;
    }
    ShowSkillEffect_Orig(pThis, pSkillEntry, a2, a3, a4, a5);
}

// The window's preview needs the same swap the world build gets, so the pane shows the tint
// being dragged rather than the saved one. Mirrors WeaponTint_Begin/EndCashEffectSwap.
//
// The swap lands in the skill's HELD list like a world cast's, so it is given the short hold
// here: a held swap with no deadline is never put back by ExpireSkillSwap, and a caller that
// forgot to end it would leave the skill tinted for everyone for the rest of the session.
bool WeaponTint_BeginSkillSwap(void* pAvatar, int skillId) {
    if (!BeginSkillSwap(reinterpret_cast<CAvatar*>(pAvatar), skillId)) return false;
    HoldSkillSwap(skillId, kSkillSwapHoldMs);
    return true;
}
// The client frame clock CreateBullet's launch/arrive pair is expressed on. NOT GetTickCount:
// `mov eax,[0xBE7B38]; mov eax,[eax+0x18]; ret`, and it reads milliseconds (the stock shooting
// path derives flight time from it as round(distance * 1.5) ms). Same address ricochetfx.cpp
// uses, for the same reason.
using t_GetFrameTime = int(__cdecl*)();
auto GetBulletFrameTime = reinterpret_cast<t_GetFrameTime>(0x00987257);

void WeaponTint_NoteBulletFlight(void* psBallUol, int nBulletItemId, int tArriveFrameTime) {
    (void)nBulletItemId;                     // kept in the signature: it names the fallback art
    int now = 0;
    __try {
        now = GetBulletFrameTime();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        now = 0;
    }
    const int flightMs = now ? (tArriveFrameTime - now) : 0;

    // Only bullets carrying skill BALL art. One without it draws from its bullet item, which
    // lives outside the skill subtree, and no window can help it -- a plain arrow fired by a
    // normal attack is one of these and is meant to stay its own colour.
    if (!psBallUol) return;
    // Against the skill whose cast queued it. Nothing is held when the cast was not tinted.
    if (!IsSkillSwapHeld(g_lastSkillSwapId)) return;
    // A bullet already past its arrival, or one claiming to fly for five seconds, is not
    // something to extend a global tree edit for.
    if (flightMs <= 0 || flightMs > 5000) return;

    // Only ever pushes the deadline OUT, so the longest bullet in a volley sets it.
    HoldSkillSwap(g_lastSkillSwapId, static_cast<DWORD>(flightMs) + kSkillSwapHoldMs);
}

void WeaponTint_EndSkillSwap() {
    SwapOutList(g_skillSwaps);               // the preview's own list, never a held one
}

// One tinted canvas, for a caller that wants to BLIT it rather than hand it to a layer.
//
// The returned pointer is only good for the current frame: the clone cache drops a colour's
// clones as soon as that colour changes. Blit it and forget it. Holding one across frames is
// what made the skill preview crash -- a layer kept these alive in its own draw list and the
// renderer walked them after they were freed.
IWzCanvasPtr WeaponTint_TintedCanvasFor(IWzCanvasPtr src, const WeaponTint& t, bool mirror) {
    if (!src) return src;
    // An identity tint still needs a clone when mirrored: there is nothing to recolour, but
    // the pixels do have to be reversed.
    if (t.IsIdentity() && !mirror) return src;
    return mirror ? GetTintedMirrored(src, t) : GetTinted(src, t);
}
void AttachWeaponTintMod() {
    // The face builder. Separate from PrepareActionLayer because the face is a separate
    // layer built by a separate function -- without this the eye tint is silently inert.
    ATTACH_HOOK(BuildFaceLayer_Orig, BuildFaceLayer_Hook);
    ATTACH_HOOK(PrepareActionLayer_Orig, PrepareActionLayer_Hook);
    // The two cash-effect builders. Both are leaf functions: the third one that reaches
    // this art, 0x0093B6A7, CALLS these two and also serves the ItemEff path, so hooking
    // it as well would double-swap.
    ATTACH_HOOK(ShowSkillEffect_Orig, ShowSkillEffect_Hook);
    ATTACH_HOOK(CashEffectShow_Orig, CashEffectShow_Hook);
    ATTACH_HOOK(CashEffectApply_Orig, CashEffectApply_Hook);
}
