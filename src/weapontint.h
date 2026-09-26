#pragma once
#include <cstdint>

class CInPacket;

// ============================================================
// weapontint.h: the HSB re-tint applied to a worn Cash weapon.
//
// A tint is a hue instruction plus a saturation and a value delta, roughly what the modern
// Coloring Prism dialog exposes as Tone / Chroma / Brightness. The identity tint (all three
// zero) means "vanilla", so no separate "is dyed" flag is needed anywhere: on the wire, in
// the DB, or in this struct.
//
// THE SIGN OF `hue` SELECTS THE SEMANTIC:
//     hue  > 0   ROTATE the sprite's own hue by that many degrees (1..359)
//     hue  < 0   SET an absolute target hue, encoded as -(degrees + 1), so -1 is 0 degrees
//                and -360 is 359
//     hue == 0   leave the hue alone
//
// Both forms are needed and neither is right for everything. A rotation preserves a
// sprite's internal hue relationships, so a two-tone item keeps both tones; an absolute
// target collapses every pixel onto one hue. Measured over 235 items, 40% are not
// essentially single-hue and 30% clearly carry several, so a system that can only do one of
// the two is wrong for a third of the wardrobe either way.
//
// ENCODING ABSOLUTE AS NEGATIVE MAKES THE SIGN A VERSION TAG. Every record written before
// this existed is positive, so it keeps rotating exactly as it did and needs no migration --
// which is what lets absolute hue be introduced later without resetting anyone's dyes. The
// encoding is what makes 0 mean "no change" rather than "absolute red", and it costs no
// database column and no packet byte: SMALLINT is signed.
// ============================================================
struct WeaponTint {
    short       hue    = 0;   // 0 none; 1..359 rotate; -(deg+1) absolute. See above.
    signed char chroma = 0;   // -100..100  saturation, percent
    signed char bright = 0;   // -100..100  value, percent

    bool IsIdentity() const { return hue == 0 && chroma == 0 && bright == 0; }
    bool IsAbsoluteHue() const { return hue < 0; }
    // The degrees this means, whichever form it is in. 0..359 either way.
    int HueDegrees() const {
        return hue < 0 ? -static_cast<int>(hue) - 1 : static_cast<int>(hue);
    }
    static short EncodeAbsoluteHue(int degrees) {
        if (degrees < 0) degrees = 0;
        if (degrees > 359) degrees = 359;
        return static_cast<short>(-(degrees + 1));
    }
    // Both forms, and nothing else. Anything outside these two bands is a decode error or a
    // hostile packet, not a colour.
    static bool IsHueValid(int hue) {
        return (hue >= 0 && hue <= 359) || (hue >= -360 && hue <= -1);
    }
    bool operator==(const WeaponTint& o) const {
        return hue == o.hue && chroma == o.chroma && bright == o.bright;
    }
    bool operator!=(const WeaponTint& o) const { return !(*this == o); }
    uint32_t Key() const {
        return (static_cast<uint32_t>(static_cast<uint16_t>(hue)) << 16)
             | (static_cast<uint32_t>(static_cast<uint8_t>(chroma)) << 8)
             |  static_cast<uint32_t>(static_cast<uint8_t>(bright));
    }
};

constexpr short kTintHueMax    = 359;
constexpr signed char kTintDeltaMin = -100;
constexpr signed char kTintDeltaMax =  100;

// ============================================================
// LOOK TINTS -- hair and eye colour, the window's Hair / Face tabs.
//
// These share the equip tint's table, its opcodes and its render hook. They are
// keyed by a KIND SENTINEL rather than by the hair/face WZ id, and that choice is
// load-bearing for four reasons:
//
//   1. Storage. Hair and eyes are not inventory rows, so an id key would imply a
//      per-(character, hairId) table; a kind key is six columns on `characters`.
//   2. The client already owns the id. Hair is AvatarLook.anHairEquip[0] and face
//      is AvatarLook.nFace, read off whichever avatar the renderer just handed
//      over -- for a REMOTE player that avatar's look is the only correct source,
//      so a server-supplied id could only agree with it or be wrong.
//   3. It deletes a duplicated classifier. With ids, CharacterSubdirOf would have
//      to reproduce the client's get_equip_data_path (0x005C94A1), including any runtime
//      widening of it; with a kind the DLL just says L"Hair" / L"Face".
//   4. Players expect a dyed colour to survive a restyle. Keyed by id it would not.
//
// 1 and 2 cannot collide with the equip ids sharing the table: those are 1xxxxxx.
// ============================================================
constexpr int kTintKey_Hair = 1;
constexpr int kTintKey_Face = 2;
// Skin is the body and arm canvases in Character/000020NN.img, NOT an equip and not a
// recolour of the client's own skin palette: the same HSB rotation every other tab uses.
constexpr int kTintKey_Skin = 3;

inline bool IsLookTintKey(int key) {
    return key == kTintKey_Hair || key == kTintKey_Face || key == kTintKey_Skin;
}

// An item's EFFECT layers are dyed separately from its body, so they need a second key
// per item. Biasing the id keeps one table and one wire format: equip ids top out at
// 1999999, so id + 10000000 cannot collide with a real id, with the two look sentinels,
// or with another item's effect key. Only ~3.4% of installed equips have effect layers
// at all (mostly cash weapons), so most items never produce one of these.
constexpr int kTintKey_EffectBias = 10000000;
// BOUNDED, deliberately. This used to be an open-ended `key >= kTintKey_EffectBias`, which
// silently claimed every value above ten million and made any future key band impossible to
// add without misclassifying it as an equip effect. Equip ids top out at 1999999, so effect
// keys top out here.
constexpr int kTintKey_EffectMax  = kTintKey_EffectBias + 1999999;
inline bool IsEffectTintKey(int key) {
    return key >= kTintKey_EffectBias && key <= kTintKey_EffectMax;
}
inline int  EffectTintKeyFor(int itemId) { return itemId + kTintKey_EffectBias; }
inline int  ItemOfEffectTintKey(int key) { return key - kTintKey_EffectBias; }

// ============================================================
// SKILL PART TINTS -- the window's Skills tab.
//
// A skill is not one sprite. Its node, Skill/<img>/skill/<id>, carries up to seventeen kinds
// of art (the caster's effect, a projectile, what the mob wears when hit, a full-screen
// overlay, ...) and each is dyed on its own. The PART is the direct child NAME, matched
// case-sensitively; several names can share a part (`hit` and `hit0`, `effect` and
// `effect_ship`). Anything not listed -- icon*, level, req, action, info, common, summon,
// afterimage, weapon, masterLevel -- is not a dyeable part and is never swapped, which is
// what keeps the skill-window icon its own colour.
//
// THIS TABLE IS A WIRE CONTRACT: the server validates the part byte of actions 5/6 against
// the same ids, and a mismatch stores a colour no client will ever draw.
// ============================================================
constexpr int kSkillPart_None       = 0;
constexpr int kSkillPart_Effect     = 1;    // "effect" + any other "effect*" not below
constexpr int kSkillPart_Effect0    = 2;
constexpr int kSkillPart_Effect1    = 3;
constexpr int kSkillPart_Effect2    = 4;
constexpr int kSkillPart_Effect3    = 5;
constexpr int kSkillPart_Ball       = 6;    // ball, ball0, ball1
constexpr int kSkillPart_Hit        = 7;    // hit, hit0, hit1
constexpr int kSkillPart_Affected   = 8;    // affected, affected0, specialAffected
constexpr int kSkillPart_Special    = 9;
constexpr int kSkillPart_Prepare    = 10;
constexpr int kSkillPart_Keydown    = 11;   // keydown, keydown0
constexpr int kSkillPart_KeydownEnd = 12;
constexpr int kSkillPart_Repeat     = 13;
constexpr int kSkillPart_Finish     = 14;
constexpr int kSkillPart_Screen     = 15;
constexpr int kSkillPart_Tile       = 16;
constexpr int kSkillPart_Mob        = 17;   // mob, mob0
constexpr int kSkillPartMin = kSkillPart_Effect;
constexpr int kSkillPartMax = kSkillPart_Mob;
constexpr int kSkillPartCount = kSkillPartMax;   // ids are 1..17, so an array needs +1

// SKILLS NEED THEIR OWN BAND because skill ids collide head-on with equip ids: 1001003,
// 2101005 and 4111004 all sit inside the equip range 1000000..1999999, so a raw skill id as
// a tint key would dye a hat.
//
// key = (part + 1) * 100000000 + skillId. The stride has to clear EIGHT-digit ids: Cygnus
// (Skill/1000..1512.img, 11001004) and Aran (Skill/2000..2112.img, 21001001) skills are
// 8 digits, and the old +30M / +40M bands put a Cygnus body key inside the fx band and an Aran
// key in no band at all. Part 1 starts at 200,000,000, clear of every equip, item-effect and
// look key; part 17 tops out at 1,899,999,999, inside int32.
constexpr int kTintKey_SkillPartStride = 100000000;
constexpr int kTintKey_SkillPartMin    = 2 * kTintKey_SkillPartStride;
inline bool IsSkillPartKey(int key) { return key >= kTintKey_SkillPartMin; }
inline int  SkillPartKeyFor(int skillId, int part) {
    return (part + 1) * kTintKey_SkillPartStride + skillId;
}
inline int  SkillOfPartKey(int key) { return key % kTintKey_SkillPartStride; }
// Not range-checked: a caller that needs a real part tests kSkillPartMin..kSkillPartMax.
inline int  PartOfPartKey(int key) { return key / kTintKey_SkillPartStride - 1; }
inline bool IsSkillPartValid(int part) {
    return part >= kSkillPartMin && part <= kSkillPartMax;
}

// Custom opcode pair from a private 0x372x block, following an even/odd
// request/reply convention.
// Server side is net.opcodes.RecvOpcode.WEAPON_TINT_ACTION / SendOpcode.WEAPON_TINT_SYNC.
constexpr unsigned short kWeaponTintActionOpcode = 0x372E;   // client -> server
constexpr unsigned short kWeaponTintSyncOpcode   = 0x372F;   // server -> client

// --- inbound (routed from the packet dispatcher) -----------------------------
void WeaponTint_HandleSync(CInPacket* p);

// --- hook installation (hook.h / AttachClientHooks) --------------------------
void AttachWeaponTintMod();

// --- item-effect layers ------------------------------------------------------
// The Effects tab's colour applied to a self-animating item-effect layer (a cape
// aura), which is built outside CAvatar::PrepareActionLayer and so is invisible to
// the hook that tints everything else. Wrap the layer build: Begin swaps tinted
// clones into Effect/ItemEff.img/<itemId>/effect and returns whether it swapped
// anything, End puts the originals back. ALWAYS call End if Begin was called, even
// when it returned false. Not reentrant.
//
// Called from itemeff.cpp's UpdateItemEff around its CUser::LoadLayer, so a cape aura
// is dyed through the same tab, the same tint key and the same database columns as
// everything else.
bool WeaponTint_BeginItemEffSwap(void* pAvatar, int itemId);
void WeaponTint_EndItemEffSwap();

// The EFFECT-layer tint that currently applies to `itemId` on `pAvatar`, as
// WeaponTint::Key() (0 = vanilla). A layer cache keyed on item / action / facing
// compares this too, so a recolour rebuilds the layer instead of waiting for the
// player to turn around. Main thread.
uint32_t WeaponTint_ItemEffTintKey(void* pAvatar, int itemId);

// The same pair for a CASH EFFECT item's art under Item/Cash/0501.img (or 0528.img).
// The window uses these around the preview avatar's own effect layer; the client's two
// cash-effect Detours use the internal form directly.
bool WeaponTint_BeginCashEffectSwap(void* pAvatar, int itemId);
void WeaponTint_EndCashEffectSwap();

// The same pair for a SKILL's effect art under Skill/<job>.img/skill/<id>. The window uses
// these around its own preview build; the client-side ShowSkillEffect Detour uses the
// internal form directly.
bool WeaponTint_BeginSkillSwap(void* pAvatar, int skillId);
void WeaponTint_EndSkillSwap();
// Is this a cash EFFECT item, the 5010000..5019999 group that sits in the Cash tab and
// plays an effect around the character? Those are dyed through the Effects tab like an
// item's glow, but they are not equips, so the window's drop gate has to admit them
// separately.
inline bool IsCashEffectItemId(int itemId) {
    return itemId >= 5010000 && itemId <= 5019999;
}

// Main thread, once per frame (driven from the per-frame update hook). Applies a
// tint the server pushed while the packet was being handled on the receive thread.
void WeaponTint_Tick();

// --- queries + control, used by coloringprism.cpp -----------------------------

// The item a Coloring Prism is being pointed at: where it lives in the inventory
// (so the server can find it) plus its id (so the client can render and verify it).
struct WeaponTintTarget {
    int invType = 0;     // InventoryType value; EQUIPPED for a worn item
    int invPos  = 0;     // position within that inventory, negative when worn
    int itemId  = 0;
    // A SKILL has no inventory address, so it fills this instead and leaves the three above
    // at zero. The two are mutually exclusive: whichever is set is what the window is aimed
    // at, and IsSet() answers for both so every "is there a target" test keeps working.
    int skillId = 0;
    bool IsSet() const { return itemId > 0 || skillId > 0; }
};

// Item id in the worn Cash-weapon slot, or 0 if the player has no Cash weapon on.
// Read straight off the local CAvatar's own look, so it needs no server round trip.
int  WeaponTint_GetLocalCashWeaponId();

// The tint the server last told us is stored on a given item, per ITEM -- every
// worn equip can carry its own.
WeaponTint WeaponTint_GetSavedFor(int itemId);

// While the prism window is open its slider values REPLACE the saved tint for the
// ONE item being dyed, so that item recolors live while the rest of the outfit
// keeps its own colours. Passing active=false drops back to the saved tint.
// The base (non-cash) weapon id an avatar is wearing, or 0. Used to pick an attack pose that
// the weapon in hand can actually play.
// Zero a canvas to fully transparent. False if the surface could not be written, which for a
// caller with a region it does not otherwise repaint means: leave that region alone.
// A projectile has been queued carrying skill BALL art, arriving at {@code tArriveFrameTime} on
// the client frame clock. Its sprite resolves on a later logic tick rather than in the call that
// queues it, so the skill's tint has to stay in the WZ tree until then -- otherwise only the
// first shot of a volley comes out dyed. No-op unless a cast's swap is currently held.
void WeaponTint_NoteBulletFlight(void* psBallUol, int nBulletItemId, int tArriveFrameTime);

bool WeaponTint_ClearCanvas(void* pCanvas, int w, int h);

// Does this equip carry glow art of its own, separate from its sprite? False means there is no
// second layer to dye, and the window greys its glow chip. Cached per item id.
bool WeaponTint_ItemHasEffectArt(int itemId);

// --- skill parts (see kSkillPart_* above) ------------------------------------
// The part a direct child of Skill/<img>/skill/<id> belongs to, or kSkillPart_None for a
// name that is not dyeable art. Case-sensitive, exactly as the contract names them.
int WeaponTint_SkillPartOfChild(const wchar_t* name);

// The DYEABLE parts this skill actually has, ascending: a part is listed only when at least
// one of its nodes holds a canvas somewhere below it. Returns how many were written (at most
// cap). Cached per skill id.
int WeaponTint_ListSkillParts(int skillId, int partsOut[], int cap);
bool WeaponTint_SkillHasPart(int skillId, int part);

// Every direct child of the skill node that maps to a listed part, in the node's own order,
// as (part, child name). A part can own several children (`hit` + `hit0`). Cached per skill.
struct WeaponTintSkillChild {
    int     part = kSkillPart_None;
    wchar_t name[24] = {};
};
int WeaponTint_ListSkillPartChildren(int skillId, WeaponTintSkillChild out[], int cap);

// "Effect", "Effect 0", ..., "Keydown End", "Mob"; L"" for an unknown part.
const wchar_t* WeaponTint_SkillPartLabel(int part);

int WeaponTint_BaseWeaponIdOf(void* pAvatar);

void WeaponTint_SetPreview(int itemId, const WeaponTint& t, bool active);
bool WeaponTint_IsPreviewActive();

// Whichever tint is currently driving the render for `itemId`.
WeaponTint WeaponTint_GetEffectiveFor(int itemId);

// Wrap a CAvatar::Init / PrepareActionLayer call that belongs to a preview avatar
// this DLL owns, so its weapon layers get the effective tint even though the
// avatar is not the one CUserLocal points at. The scope is bound to that exact
// avatar; it must never make another avatar inherit a live preview tint.
// Legacy previews that only need the saved-tint table may omit the avatar. They
// never opt into the live Coloring Prism slider value.
void WeaponTint_BeginForcedScope();
void WeaponTint_BeginForcedScope(void* pAvatar);
// Salon previews can supply the saved hair and eye tints for their look. Identity
// values are meaningful here: they deliberately override the player's live tint.
void WeaponTint_BeginForcedScope(void* pAvatar, const WeaponTint& hairTint,
                                 const WeaponTint& faceTint);
void WeaponTint_EndForcedScope();

// CAvatar builds its face layer asynchronously, after CAvatar::Init has returned.
// Keep the Coloring Prism's live tint bound to its exact preview avatar for that
// deferred build; this is deliberately not a global preview flag.
void WeaponTint_BindPreviewAvatar(void* pAvatar);
void WeaponTint_UnbindPreviewAvatar(void* pAvatar);
void WeaponTint_InstallPreviewFaceTint(void* pAvatar);

// Ask the client to rebuild the local player's avatar layers so a tint change is
// visible without waiting for the next action change. Best-effort.
void WeaponTint_RefreshLocalAvatar();

// Adopt `t` as the saved tint without waiting for the server to say so. Called on
// Confirm: the window closes immediately, and without this the weapon would snap
// back to its old color for the length of the round trip. The server's reply
// snapshot is authoritative and overwrites this either way.
void WeaponTint_AdoptOptimistic(int itemId, const WeaponTint& t);

// --- outbound -----------------------------------------------------------------
void WeaponTint_RequestSnapshot();
// `layer` is 0 for the item's body and 1 for its EFFECT sprites, which carry their own
// tint. The server stores them in different columns; everything else about the two is
// identical, which is why this is a byte rather than a second pair of actions.
constexpr int kTintLayer_Body    = 0;
constexpr int kTintLayer_Effects = 1;
void WeaponTint_SendApply(const WeaponTintTarget& target, const WeaponTint& t, int prismPos, int layer);
void WeaponTint_SendRestore(const WeaponTintTarget& target, int prismPos, int layer);

// Look (hair / eye) variants. They carry a KIND instead of a WeaponTintTarget,
// because hair and eyes have no inventory address for the server to re-verify --
// the server reads the character's own hair/face instead. Separate ACTIONS on the
// same opcode, since reusing apply/restore would drive them through a resolve()
// that demands a Cash equip at a given inventory position.
// Skills tab. A skill is named by its ID: it has no inventory address for the server to
// re-derive it from, so the server verifies instead that the character knows the skill.
// `part` is a kSkillPart_* id (1..17) and is REQUIRED: the trailing byte of actions 5 / 6.
void WeaponTint_SendApplySkill(int skillId, const WeaponTint& t, int prismPos, int part);
void WeaponTint_SendRestoreSkill(int skillId, int prismPos, int part);

void WeaponTint_SendApplyLook(int kind, const WeaponTint& t, int prismPos);
void WeaponTint_SendRestoreLook(int kind, int prismPos);

// The hair / face ids the LOCAL player is currently wearing, or 0. Read off the
// local CAvatar's own look, so no server round trip is needed to preview them.
int WeaponTint_GetLocalHairId();
int WeaponTint_GetLocalFaceId();
int WeaponTint_GetLocalSkinId();

// Result of the last APPLY / RESTORE the server answered, for the window to report.
enum WeaponTintResult {
    kTintResult_None = 0,
    kTintResult_Ok,
    kTintResult_NoCashWeapon,
    kTintResult_NoItem,
    kTintResult_NotTinted,
    kTintResult_Failed,
};
WeaponTintResult WeaponTint_TakeLastResult();   // reads and clears
