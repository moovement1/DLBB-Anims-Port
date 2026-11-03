// equip_anim.asi — Dying Light 1 weapon equip animation plugin
//
// Makes weapon switching work like Dying Light: Bad Blood: the old weapon is swapped out instantly
// (no lower/raise lerp) and the new weapon's "Show" animation brings it up.
//
// How DL1 switches weapons (gamedll, build 24076292):
//   SetWeaponSlot puts the new weapon in the *requested* hand nodes (weaponMgr+0x16B0 / +0x16D0).
//   Every frame FUN_180BAEE40(weaponMgr, instant) calls BlendCompute(weaponMgr, hand, instant) for
//   both hands. BlendCompute lowers the hand weight (weaponMgr+0x17AC + hand*4) over 0.2 s, then
//   FUN_180BAEBE0 moves the requested item into the *active* node (+0x1630 + hand*0x20) and the
//   weight is raised again. With instant != 0 both durations are 0: swap and raise in one frame.
//
// How it works:
//   1. SetWeaponSlot succeeds with a new slot: remember the weapon manager and the item in hand.
//      If the requested weapon's AnimPrefix has a Show track (InstantPrefixes), force instant
//      switching for that manager.
//   2. Each AnimUpdate of the FPP animCtrl whose +0x50 is that manager: once the active item has
//      changed, resolve the state "Show". The engine resolver composes
//      "Fpp_" + <weapon AnimPrefix> + "_Show" itself (fpp_knife_show, fpp_stick2heavy_show, ...);
//      weapons without such a track resolve to the sentinel and are left alone.
//   3. Right after AnimUpdate the FPP vis updates its two hand tracks (vtable 0xAE8 / 0xAF0): they
//      copy the main track, then per-hand logic may replace it. For the anim's duration the plugin
//      finishes them like Bad Blood's Show/Hide handler (FUN_180D7C8C0: stateId = Show, blend -1,
//      speed 1, no loop). The main track (camera, body, jump/land) is left to the game, as in Bad
//      Blood: arm bones come from the hand tracks, everything else (camera, weapon holder bones the
//      equip anims do not key) from the weapon's own stand/move state.
//      On the first frame the hand nodes' restart flag (MultislotAnimNode +0x1C) is set, so a Show
//      that follows the same Show (two weapons with the same AnimPrefix) starts again from 0.
//      MainTrack=1 additionally plays the Show through AnimUpdate's first branch (weaponMgr action
//      state *(+0xBF8): +0x10 active, +0x38 phase, +0x44 stateId) — the v4/v5 behaviour.
//   4. Like Bad Blood, the weapon controllers ignore attack/aim input while the Show plays
//      (OnInput, vtable 0xB0 of WeaponFire/Melee/BowController).
//   5. Like Bad Blood, no Show when the weapon is put away during the switch (climbing, vaulting,
//      medkit...): BlendCompute then leaves the hand's target node (+0x1670 + hand*0x20) empty while
//      a weapon is requested. The weapon then changes directly; a Show already playing stops.
//   6. Bad Blood's special moves use code DL1 already has, gated by skills: hold attack with a light
//      weapon (ChargeAttack), with a heavy one (WhirlwindCharge), head stomp on a lying enemy
//      (StompLevel), attack while falling on an enemy (JumpAttackHit: with the mod's data, Bad Blood's
//      drop attack), ground pound with a heavy weapon (GroundPound). The plugin forces these player
//      properties each frame (Unlock* in the ini) so the moves work without the skill.
//   7. During the attack while falling the camera follows the enemy like Bad Blood's (JumpAttackCamera):
//      from the contact until the grab ends, the player's look pitch is aimed at the enemy's chest.
//      Its impact blows the head off (JumpAttackHead): GRAB_DESTROY_HEAD acts even without the skill.
//
// The Show tracks, Bad Blood's attack tracks and their .anm2 files are added to common_anims_PC.rpack
// by data\build_equip_rpack.py. Without them every Show lookup returns the sentinel and nothing plays.
//
// All game code is located by signature. If a required signature or a layout check fails the
// plugin logs it and stays off.
//
// Object layout (verified in gamedll):
//   SetWeaponSlot param_1 = weaponMgr  (+0x2A50 current slot, +0x16B0/+0x16D0 requested items)
//   weaponMgr vtable[0x798/8](mgr, hand) -> active item (node +0x1630 + hand*0x20)
//   weaponMgr + 0x1670 + hand*0x20: node of the item BlendCompute brings up (empty = weapon put away)
//   item+0x58 vtable[0x658/8] -> EngineStr* AnimPrefix
//   AnimUpdate    param_1 = animCtrl   (+0x40 IModelObject holder, +0x50 weaponMgr)
//   *(animCtrl+0x1A8)[i]: FPP anim tree built by vtable 0x8C8 (FUN_180C156A0): 0 MoveAnimNode,
//   1 main MultislotAnimNode (= vtable[0x8F0/8]()), 2 / 3 hand Multislot nodes, 4 / 5 ItemAnimNode
//   (weapon model, follows its hand node), 6 MimicAnimNode. Node +0xD8 -> track, +0x1C restart
//   flag, +0x24 playing stateId. Track: +0x10 stateId, +0x1C speed, +0x20 blend time, +0x2C loop
//   animCtrl + 0x182C int32 one-shot stateId (landing), + 0x1830 float timer (seconds)
//   animCtrl vtable[0xD38/8](animCtrl, &stateId) -> float duration
//   player properties (getter FUN_180D8CDE0): *(*(*(player+0xE58)+0xD0) + id*8) -> variable, value at
//   +8: StompLevel 0x608 (float, -1 = off), WhirlwindCharge 0x1D20, ChargeAttack 0x1D28 (bit 0)

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "MinHook.h"

// ── Logging ───────────────────────────────────────────────────────────────────
static bool s_logEnabled = true;
static char s_logPath[MAX_PATH] = "equip_anim.log";

static void Log(const char* fmt, ...) {
    if (!s_logEnabled) return;
    char buf[512];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    FILE* f = fopen(s_logPath, "a");
    if (f) { fputs(buf, f); fputs("\n", f); fclose(f); }
}

// ── Config (equip_anim.ini next to the .asi) ─────────────────────────────────
static bool  s_enabled         = true;
static char  s_animName[64]    = "Show";
static bool  s_instantSwitch   = true;   // skip the lower/raise lerp for weapons with a Show track
static char  s_instantPrefixes[512] =
    "knife,stick,stick2close,stick2heavy,gnome,beretta,schofield,sixshooter,walker,singleshot,"
    "shotgun,shotgun_pa,shotgun_sa,m16,ak47,railgun,grenadelauncher,chainsaw,submachinegun,"
    "submachinegun_alt,crossbow,bow,fist,fistinfection,shield";
static float s_maxDuration     = 3.0f;
static bool  s_handTracks      = true;   // play the Show on both hand tracks (Bad Blood)
static bool  s_mainTrack       = false;  // also play it on the main track (v4/v5, moves the camera)
static bool  s_blockAttacks    = true;   // weapon input ignored while the Show plays (Bad Blood)
static bool  s_unlockCharge    = true;   // hold attack with light weapons without the skill
static bool  s_unlockWhirlwind = true;   // hold attack with heavy weapons without the skill
static bool  s_unlockStomp     = true;   // head stomp on lying enemies without the skill
static bool  s_unlockJumpAttack  = true; // attack while falling on an enemy without the skill
static bool  s_unlockGroundPound = true; // ground pound with heavy weapons without the skill
static float s_groundPoundMinHeight = 3.0f; // Bad Blood's value (DL1: 1.1), 0 = keep the game's
static bool  s_jumpAttackClick = true;   // a click in the air is enough, the button need not stay down
static bool  s_jumpAttackLog   = true;   // logs why the attack while falling starts or not
static bool  s_jumpCamera      = true;   // the camera follows the enemy during that move
static uint32_t s_jumpCameraMs = 1500;   // ... until this long after the landing (0 = until the move ends)
static bool  s_jumpHead        = true;   // its kick blows the head off (GRAB_DESTROY_HEAD in the mod's data)

static HMODULE s_self = nullptr;

static void LoadConfig() {
    char dir[MAX_PATH] = {};
    GetModuleFileNameA(s_self, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0;
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%sequip_anim.ini", dir);
    snprintf(s_logPath, sizeof(s_logPath), "%sequip_anim.log", dir);

    s_enabled       = GetPrivateProfileIntA("General", "Enabled", 1, ini) != 0;
    s_logEnabled    = GetPrivateProfileIntA("General", "Log", 1, ini) != 0;
    s_instantSwitch = GetPrivateProfileIntA("General", "InstantSwitch", 1, ini) != 0;
    s_handTracks    = GetPrivateProfileIntA("General", "HandTracks", 1, ini) != 0;
    s_mainTrack     = GetPrivateProfileIntA("General", "MainTrack", 0, ini) != 0;
    s_blockAttacks  = GetPrivateProfileIntA("General", "BlockAttacks", 1, ini) != 0;
    s_unlockCharge    = GetPrivateProfileIntA("General", "UnlockCharge", 1, ini) != 0;
    s_unlockWhirlwind = GetPrivateProfileIntA("General", "UnlockWhirlwind", 1, ini) != 0;
    s_unlockStomp     = GetPrivateProfileIntA("General", "UnlockStomp", 1, ini) != 0;
    s_unlockJumpAttack  = GetPrivateProfileIntA("General", "UnlockJumpAttack", 1, ini) != 0;
    s_unlockGroundPound = GetPrivateProfileIntA("General", "UnlockGroundPound", 1, ini) != 0;
    char minHeight[32];
    GetPrivateProfileStringA("General", "GroundPoundMinHeight", "3.0", minHeight, sizeof(minHeight), ini);
    s_groundPoundMinHeight = (float)atof(minHeight);
    s_jumpAttackClick = GetPrivateProfileIntA("General", "JumpAttackClick", 1, ini) != 0;
    s_jumpAttackLog   = GetPrivateProfileIntA("General", "JumpAttackLog", 1, ini) != 0;
    s_jumpCamera      = GetPrivateProfileIntA("General", "JumpAttackCamera", 1, ini) != 0;
    char cameraTime[32];
    GetPrivateProfileStringA("General", "JumpAttackCameraTime", "1.5", cameraTime, sizeof(cameraTime), ini);
    s_jumpCameraMs = (uint32_t)(atof(cameraTime) > 0 ? atof(cameraTime) * 1000.0 : 0);
    s_jumpHead        = GetPrivateProfileIntA("General", "JumpAttackHead", 1, ini) != 0;
    GetPrivateProfileStringA("General", "AnimName", "Show", s_animName, sizeof(s_animName), ini);
    GetPrivateProfileStringA("General", "InstantPrefixes", s_instantPrefixes, s_instantPrefixes,
                             sizeof(s_instantPrefixes), ini);
    char tmp[32];
    GetPrivateProfileStringA("General", "MaxDuration", "3.0", tmp, sizeof(tmp), ini);
    s_maxDuration = (float)atof(tmp);
    if (s_maxDuration <= 0.0f) s_maxDuration = 3.0f;
}

// ── Signature scanning ────────────────────────────────────────────────────────
static uintptr_t s_base = 0;
static uintptr_t s_textBegin = 0, s_textEnd = 0;
static uintptr_t s_rdataBegin = 0, s_rdataEnd = 0;

static bool FindSections(uintptr_t base) {
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt  = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        uintptr_t b = base + sec->VirtualAddress, e = b + sec->Misc.VirtualSize;
        if (memcmp(sec->Name, ".text\0", 6) == 0)  { s_textBegin = b;  s_textEnd = e; }
        if (memcmp(sec->Name, ".rdata\0", 7) == 0) { s_rdataBegin = b; s_rdataEnd = e; }
    }
    return s_textBegin && s_rdataBegin;
}

static uintptr_t RipTarget(uintptr_t insn, int dispOffset, int insnLen) {
    return insn + insnLen + *(int32_t*)(insn + dispOffset);
}

// "48 8B ?? 05" style pattern over [begin, end) (default .text). Returns the match only if it is
// unique, or, with ripRef, the unique match whose rel32 at ripRef (instruction ending at ripRef+4)
// targets ripTarget.
static uintptr_t Scan(const char* pattern, int ripRef = -1, uintptr_t ripTarget = 0,
                      uintptr_t begin = 0, uintptr_t end = 0) {
    uint8_t bytes[128]; bool wild[128]; int n = 0;
    for (const char* p = pattern; *p && n < 128;) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { wild[n] = true; bytes[n++] = 0; p += (p[1] == '?') ? 2 : 1; continue; }
        wild[n] = false; bytes[n++] = (uint8_t)strtoul(p, nullptr, 16); p += 2;
    }
    if (!begin) { begin = s_textBegin; end = s_textEnd; }
    uintptr_t found = 0;
    for (uintptr_t a = begin; a + n <= end; a++) {
        int i = 0;
        while (i < n && (wild[i] || *(uint8_t*)(a + i) == bytes[i])) i++;
        if (i != n) continue;
        if (ripRef >= 0 && RipTarget(a + ripRef, 0, 4) != ripTarget) continue;
        if (found) return 0;  // ambiguous
        found = a;
    }
    return found;
}

// Null-delimited string in .rdata.
static uintptr_t FindString(const char* s) {
    size_t n = strlen(s) + 1;
    for (uintptr_t a = s_rdataBegin + 1; a + n <= s_rdataEnd; a++)
        if (*(char*)(a - 1) == 0 && memcmp((void*)a, s, n) == 0) return a;
    return 0;
}

// True if `code` contains `bytes` within its first `len` bytes.
static bool CodeHas(uintptr_t code, size_t len, const char* bytes, size_t n) {
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp((void*)(code + i), bytes, n) == 0) return true;
    return false;
}

// ── Game functions ────────────────────────────────────────────────────────────
typedef uint64_t (__fastcall* FnSetWeaponSlot)(void* weaponMgr, int newSlot, bool flag);
typedef uint64_t (__fastcall* FnAnimUpdate)(void* animCtrl);
typedef uint64_t (__fastcall* FnBlendCompute)(void* weaponMgr, int hand, char instant);
typedef int32_t* (__fastcall* FnResolveStateId)(void* animCtrl, int32_t* stateIdOut, void* nameStr, void* iModelObj, bool flag);
typedef void*    (__fastcall* FnGetAnimPrefix)(void* animCtrl, int slot);
typedef void     (__fastcall* FnHandTrack)(void* animCtrl);
typedef void     (__fastcall* FnWeaponInput)(void* ctrl, uint32_t action, float value, char flag);

static FnSetWeaponSlot  s_origSlot         = nullptr;
static FnAnimUpdate     s_origAnimUpdate   = nullptr;
static FnBlendCompute   s_origBlendCompute = nullptr;
static FnHandTrack      s_origHandOff      = nullptr;  // FPP vis vtable 0xAE8: hand track 3 (hand 1, weight +0x17B0)
static FnHandTrack      s_origHandMain     = nullptr;  // FPP vis vtable 0xAF0: hand track 2 (hand 0, weight +0x17AC)
static FnWeaponInput    s_origFireInput    = nullptr;  // WeaponFireController(Left/Right) vtable 0xB0
static FnWeaponInput    s_origMeleeInput   = nullptr;  // WeaponMeleeController vtable 0xB0
static FnWeaponInput    s_origBowInput     = nullptr;  // WeaponBowController vtable 0xB0
static FnResolveStateId s_resolve          = nullptr;
static FnGetAnimPrefix  s_getPrefix        = nullptr;
static int32_t**        s_sentinelPtr      = nullptr;  // engine global holding the invalid TAnimId
static void*            s_fppPrefixFn      = nullptr;  // FPP animCtrl vtable[0xAA0/8]: appends "Fpp_"
static int              s_handItemSlot     = -1;       // weaponMgr vtable slot: active item (0 = main, 1 = off)
static int              s_itemPrefixSlot   = -1;       // item+0x58 vtable slot: EngineStr* AnimPrefix
static bool             s_requestedNodesOk = false;    // weaponMgr+0x16B0/+0x16D0 confirmed in SetWeaponSlot
static bool             s_targetNodesOk    = false;    // weaponMgr+0x1670 confirmed in BlendCompute
static bool             s_canInstant       = false;
static int              s_actionOff        = 0;        // weaponMgr offset of the action state (0xBF8)

// ── Chrome Engine string struct ───────────────────────────────────────────────
struct EngineStr { const char* ptr; uint32_t length; uint32_t capacity; };

// ── State ─────────────────────────────────────────────────────────────────────
static void* volatile    s_pendingMgr    = nullptr;
static void* volatile    s_pendingItem   = nullptr;  // active item when the switch started
static volatile uint64_t s_pendingStart  = 0;
static volatile uint64_t s_pendingUntil  = 0;
static void* volatile    s_instantMgr    = nullptr;  // BlendCompute runs with instant=1 for this mgr...
static volatile uint64_t s_instantUntil  = 0;        // ...until this tick
static void* volatile    s_switchMgr     = nullptr;  // weapon manager of the last switch
static void* volatile    s_requestedItem = nullptr;  // weapon the current switch brings up
static volatile int      s_requestedHand = 0;        // ...in this hand (0 main, 1 off)
static volatile uint64_t s_hiddenAt      = 0;        // tick the weapon was put away during the switch
static void* volatile    s_showCtrl      = nullptr;  // FPP animCtrl playing the Show state...
static volatile int32_t  s_showId        = 0;
static volatile uint64_t s_showUntil     = 0;        // ...until this tick
static volatile uint64_t s_showArmedAt   = 0;
static volatile uint32_t s_showMs        = 0;
static volatile bool     s_showThisFrame = false;    // the Show drives the tracks this frame
static volatile int      s_showFrames    = 0;        // frames played
static volatile int      s_handFixes     = 0;        // hand-track updates that wanted their own state
static volatile int      s_inputBlocked  = 0;        // weapon input events dropped during the Show
static volatile bool     s_waitLogged    = false;

static bool IsPtr(const void* p) {
    uintptr_t v = (uintptr_t)p;
    return v > 0x10000 && v < 0x000F000000000000ULL;
}

static int32_t Sentinel() { return **s_sentinelPtr; }

// The player's TPP body controller points at the same weaponMgr; only the FPP one has Show tracks.
static bool IsFppCtrl(void* animCtrl) {
    if (!s_fppPrefixFn) return true;
    __try { return (*(void***)animCtrl)[0xAA0 / 8] == s_fppPrefixFn; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Same main/off-hand choice as GetAnimPrefix (FUN_180C66230).
static void* PickHand(void* main, void* off) { return (off && !main) ? off : main; }

// Item the animation system treats as "in hand".
static void* ActiveItem(void* weaponMgr) {
    __try {
        auto get = (void*(__fastcall*)(void*, int))(*(void***)weaponMgr)[s_handItemSlot];
        void* off  = get(weaponMgr, 1);
        void* main = get(weaponMgr, 0);
        return PickHand(main, off);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Item SetWeaponSlot asked for (list nodes: [0] = list head, [1] = object).
static void* RequestedItem(void* weaponMgr, int* hand = nullptr) {
    __try {
        char* m = (char*)weaponMgr;
        void* main = *(void**)(m + 0x16B0) ? *(void**)(m + 0x16B8) : nullptr;
        void* off  = *(void**)(m + 0x16D0) ? *(void**)(m + 0x16D8) : nullptr;
        if (hand) *hand = (off && !main) ? 1 : 0;
        return PickHand(main, off);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool ItemPrefix(void* item, char* out, size_t outSize) {
    out[0] = 0;
    if (!IsPtr(item) || s_itemPrefixSlot < 0) return false;
    __try {
        void* comp = (char*)item + 0x58;
        auto ps = (EngineStr*)((void*(__fastcall*)(void*))(*(void***)comp)[s_itemPrefixSlot])(comp);
        if (!IsPtr(ps) || !IsPtr(ps->ptr) || ps->length >= outSize) return false;
        memcpy(out, ps->ptr, ps->length); out[ps->length] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool PrefixListed(const char* prefix) {
    size_t n = strlen(prefix);
    if (!n) return false;
    for (const char* p = s_instantPrefixes; *p;) {
        while (*p == ' ' || *p == ',') p++;
        const char* e = p;
        while (*e && *e != ',') e++;
        const char* t = e;
        while (t > p && t[-1] == ' ') t--;
        if ((size_t)(t - p) == n && _strnicmp(p, prefix, n) == 0) return true;
        p = e;
    }
    return false;
}

// ── SetWeaponSlot hook ────────────────────────────────────────────────────────
// Called when the switch starts: the old weapon is still active and the new one only requested.
static uint64_t __fastcall hkSetWeaponSlot(void* weaponMgr, int newSlot, bool flag) {
    uintptr_t caller = (uintptr_t)_ReturnAddress();
    int prevSlot = -1;
    __try { prevSlot = *(int*)((char*)weaponMgr + 0x2A50); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    uint64_t ret = s_origSlot(weaponMgr, newSlot, flag);
    bool ok = (uint8_t)ret != 0;
    if (s_enabled && ok && newSlot >= 0 && newSlot != prevSlot) {
        uint64_t now = GetTickCount64();
        void* active    = s_handItemSlot >= 0 ? ActiveItem(weaponMgr) : nullptr;
        int requestedHand = 0;
        void* requested = s_requestedNodesOk ? RequestedItem(weaponMgr, &requestedHand) : nullptr;
        // switching back to the weapon that is still active (old one not lowered yet): show it again
        s_pendingItem   = (requested && requested == active) ? nullptr : active;
        s_switchMgr     = weaponMgr;
        s_requestedItem = requested;
        s_requestedHand = requestedHand;
        s_hiddenAt      = 0;
        s_pendingStart  = now;
        s_pendingUntil  = now + 2000;
        s_pendingMgr    = weaponMgr;

        char prefix[64] = "";
        bool instant = false;
        if (s_canInstant && s_origBlendCompute && ItemPrefix(requested, prefix, sizeof(prefix)) &&
            PrefixListed(prefix)) {
            s_instantUntil = now + 2000;
            s_instantMgr   = weaponMgr;
            instant = true;
        }
        Log("[SetWeaponSlot] mgr=%p slot %d -> %d, requested '%s'%s (from RVA 0x%llX)", weaponMgr, prevSlot,
            newSlot, prefix, instant ? " (instant)" : "", (unsigned long long)(caller - s_base));
    }
    return ret;
}

static void TraceShow(void* animCtrl, const char* prefix);

// Resolve the Show state for the weapon currently in hand and start playing it.
static void ArmEquip(void* animCtrl) {
    char prefix[64] = "?";
    if (s_getPrefix) {
        __try {
            auto ps = (EngineStr*)s_getPrefix(animCtrl, -1);
            if (IsPtr(ps) && IsPtr(ps->ptr) && ps->length < sizeof(prefix)) {
                memcpy(prefix, ps->ptr, ps->length); prefix[ps->length] = 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    void* sub40 = (char*)animCtrl + 0x40;
    void* iModelObj = nullptr;
    __try {
        void** vt = *(void***)sub40;
        iModelObj = ((void*(__fastcall*)(void*))vt[1])(sub40);
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("  ERR: IModelObject threw"); return; }
    if (!IsPtr(iModelObj)) { Log("  ERR: no IModelObject"); return; }

    EngineStr es = { s_animName, (uint32_t)strlen(s_animName), (uint32_t)strlen(s_animName) };
    int32_t sentinel = Sentinel();
    int32_t stateId = sentinel;
    __try { s_resolve(animCtrl, &stateId, &es, iModelObj, false); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("  ERR: resolver threw"); return; }
    if (stateId == sentinel) {
        Log("  prefix '%s': no Fpp_%s_%s track, skipped", prefix, prefix, s_animName);
        return;
    }

    float duration = 0.0f;
    __try {
        void** vt = *(void***)animCtrl;
        duration = ((float(__fastcall*)(void*, int32_t*))vt[0xD38 / 8])(animCtrl, &stateId);
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("  ERR: duration threw"); return; }
    if (!(duration > 0.0f && duration <= s_maxDuration)) {
        Log("  prefix '%s': bad duration %.3f, skipped", prefix, duration);
        return;
    }

    uint64_t now = GetTickCount64();
    s_showId        = stateId;
    s_showMs        = (uint32_t)(duration * 1000.0f);
    s_showArmedAt   = now;
    s_showUntil     = now + s_showMs;
    s_showFrames    = 0;
    s_handFixes     = 0;
    s_inputBlocked  = 0;
    s_waitLogged    = false;
    s_showThisFrame = false;
    s_showCtrl      = animCtrl;
    Log("  prefix '%s': Fpp_%s_%s id=0x%X %.3fs", prefix, prefix, s_animName, stateId, duration);
    TraceShow(animCtrl, prefix);
}

static const char* const kStopReason[] = { "done", "weapon put away", "game action took over",
                                           "game action never ended" };

static void StopShow(void* animCtrl, int reason) {
    s_showCtrl      = nullptr;
    s_showThisFrame = false;
    Log("  Show %s: %d frames in %llu ms", kStopReason[reason], s_showFrames,
        (unsigned long long)(GetTickCount64() - s_showArmedAt));
    if (s_handFixes) Log("    %d hand-track updates had their own state", s_handFixes);
    if (s_inputBlocked) Log("    %d weapon input events ignored", s_inputBlocked);
    // MainTrack: a landing one-shot started during the Show did not count down: drop it
    if (s_mainTrack) {
        __try {
            int32_t* oneShot = (int32_t*)((char*)animCtrl + 0x182C);
            if (*oneShot != Sentinel()) *oneShot = Sentinel();
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

// True if the game's own action state (medkit, interaction...) is active.
static bool GameActionActive(char* action, int32_t* gameId) {
    __try {
        if (!action[0x10]) return false;
        *gameId = *(int32_t*)(action + 0x44 + *(int32_t*)(action + 0x38) * 0xC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
}

// MainTrack: runs AnimUpdate with the weapon manager's action state set to "Show, phase 0" so that
// its first, top-priority branch plays the Show state on the main track.
struct ActionState { uint8_t active; int32_t phase; int32_t id0; };

static void BeginShow(char* action, ActionState* saved) {
    __try {
        saved->active = action[0x10];
        saved->phase  = *(int32_t*)(action + 0x38);
        saved->id0    = *(int32_t*)(action + 0x44);
        *(int32_t*)(action + 0x44) = s_showId;
        *(int32_t*)(action + 0x38) = 0;
        action[0x10] = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void EndShow(char* action, const ActionState* saved) {
    __try {
        action[0x10] = saved->active;
        *(int32_t*)(action + 0x38) = saved->phase;
        *(int32_t*)(action + 0x44) = saved->id0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Skill-gated moves ─────────────────────────────────────────────────────────
static bool s_unlockChargeOk = false, s_unlockWhirlwindOk = false, s_unlockStompOk = false;
static bool s_unlockJumpAttackOk = false, s_unlockGroundPoundOk = false;
static bool s_unlockLogged = false, s_unlockFailLogged = false;
static char** s_propsGlobal = nullptr;   // the game's global property holder, read from the stomp getter

// Variable behind a player property, same path as the game's getters.
static char* PropertyVar(char* props, int offset) {
    char* var = *(char**)(props + offset);
    return IsPtr(var) ? var : nullptr;
}

// Same choice as the game's getters (FUN_180D8CDE0). PlayerDI's vtable+0x9C8 returns false, so the
// player reads the global holder and its own +0xE58 stays null.
static char* PropertyHolder(char* player) {
    if (player[0x50] && !((bool(__fastcall*)(void*))(*(void***)player)[0x9C8 / 8])(player))
        return *s_propsGlobal;
    return *(char**)(player + 0xE58);
}

static void ApplyUnlocks(void* animCtrl) {
    __try {
        char* player = *(char**)((char*)animCtrl + 0x50);
        if (!IsPtr(player)) return;
        char* holder = PropertyHolder(player);
        char** propsPtr = IsPtr(holder) ? *(char***)(holder + 0xD0) : nullptr;
        if (!IsPtr(propsPtr) || !IsPtr(*propsPtr)) {
            if (!s_unlockFailLogged && !s_unlockLogged) {
                s_unlockFailLogged = true;
                Log("Skills not reachable yet: player %p, flag %d, holder %p", player, player[0x50], holder);
            }
            return;
        }
        char* props = *propsPtr;

        char* charge      = s_unlockChargeOk      ? PropertyVar(props, 0x1D28) : nullptr;
        char* whirlwind   = s_unlockWhirlwindOk   ? PropertyVar(props, 0x1D20) : nullptr;
        char* stomp       = s_unlockStompOk       ? PropertyVar(props, 0x608)  : nullptr;
        char* jumpAttack  = s_unlockJumpAttackOk  ? PropertyVar(props, 0x1E00) : nullptr;
        char* groundPound = s_unlockGroundPoundOk ? PropertyVar(props, 0x1100) : nullptr;
        char* minHeight   = s_unlockGroundPoundOk ? PropertyVar(props, 0x1128) : nullptr;
        if (!s_unlockLogged) {
            s_unlockLogged = true;
            Log("Skills before unlock: ChargeAttack %d, WhirlwindCharge %d, StompLevel %.0f, "
                "JumpAttackHit %d, GroundPound %.0f, GroundPoundMinHeight %.1f",
                charge ? (charge[8] & 1) : -1, whirlwind ? (whirlwind[8] & 1) : -1,
                stomp ? *(float*)(stomp + 8) : -99.0f, jumpAttack ? (jumpAttack[8] & 1) : -1,
                groundPound ? *(float*)(groundPound + 8) : -99.0f, minHeight ? *(float*)(minHeight + 8) : -99.0f);
        }
        if (charge)     charge[8] |= 1;
        if (whirlwind)  whirlwind[8] |= 1;
        if (stomp && *(float*)(stomp + 8) < 1.0f) *(float*)(stomp + 8) = 1.0f;  // StompFinisher_Level1
        if (jumpAttack) jumpAttack[8] |= 1;
        if (groundPound && *(float*)(groundPound + 8) < 1.0f) *(float*)(groundPound + 8) = 1.0f;
        if (minHeight && s_groundPoundMinHeight > 0.0f) *(float*)(minHeight + 8) = s_groundPoundMinHeight;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Track trace ───────────────────────────────────────────────────────────────
// After a contact in the air, and during a firearm's Show: logs what the FPP tracks and the grab controller
// (player+0xBB8) do for 5 s, one line per change. A track id is the index of the track in the sorted
// anims_player bank; -1 means the name did not resolve. Hands: wanted (track +0x10) / playing (node +0x24).
static char*    s_tracePlayer = nullptr;
static uint64_t s_traceStart  = 0;
static int      s_traceLines  = 0;
static int      s_showTraces  = 0;
static int32_t  s_traceLast[10] = {};
static char* TrackOf(void* holder);

static void TraceStart(char* player) {
    s_tracePlayer = player;
    s_traceStart = GetTickCount64();
    memset(s_traceLast, 0, sizeof(s_traceLast));
}

// Firearm Shows (the pistol's is not seen in game): trace the first 10.
static void TraceShow(void* animCtrl, const char* prefix) {
    static const char* const kMelee[] = { "knife", "stick", "stick2close", "stick2heavy", "gnome", "fist",
                                          "fistinfection", "shield", nullptr };
    if (!s_logEnabled || s_showTraces >= 10 || s_traceLines >= 400) return;
    for (const char* const* m = kMelee; *m; m++)
        if (_stricmp(prefix, *m) == 0) return;
    s_showTraces++;
    __try { TraceStart(*(char**)((char*)animCtrl + 0x50)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { s_tracePlayer = nullptr; }
}

static void TraceJump(void* animCtrl) {
    __try {
        uint64_t now = GetTickCount64();
        if (now - s_traceStart > 5000 || s_traceLines >= 400 || !IsFppCtrl(animCtrl)) return;
        char* player = *(char**)((char*)animCtrl + 0x50);
        if (player != s_tracePlayer) return;
        void* mainHolder = ((void*(__fastcall*)(void*))(*(void***)animCtrl)[0x8F0 / 8])(animCtrl);
        char* holders = *(char**)((char*)animCtrl + 0x1A8);
        char* node0 = *(char**)(holders + 0x10);
        char* node1 = *(char**)(holders + 0x18);
        char* main  = TrackOf(mainHolder);
        char* hand0 = TrackOf(node0);
        char* hand1 = TrackOf(node1);
        char* grab  = *(char**)(player + 0xBB8);
        void** vt   = *(void***)player;
        int32_t cur[10] = {
            *(int32_t*)(main + 0x10), *(int32_t*)((char*)mainHolder + 0x24), *(int32_t*)(hand0 + 0x10),
            *(int32_t*)(hand1 + 0x10), grab[0x10], *(int32_t*)(grab + 0xCC),
            ((int(__fastcall*)(void*))vt[0x368 / 8])(player), ((int(__fastcall*)(void*))vt[0x370 / 8])(player),
            *(int32_t*)(node0 + 0x24), *(int32_t*)(node1 + 0x24) };
        if (memcmp(cur, s_traceLast, sizeof(cur)) == 0) return;
        memcpy(s_traceLast, cur, sizeof(cur));
        s_traceLines++;
        float buf[16];
        float* pos = ((float*(__fastcall*)(void*, float*))vt[0x270 / 8])(player, buf);
        Log("  +%llu ms: main 0x%X (playing 0x%X), hands 0x%X/0x%X (playing 0x%X/0x%X), grab active %d, phase %d, "
            "type %d, set %d, y %.2f", (unsigned long long)(now - s_traceStart), cur[0], cur[1], cur[2], cur[3],
            cur[8], cur[9], cur[4], cur[5], cur[6], cur[7], pos[1]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ── Camera during the attack while falling ────────────────────────────────────
// Bad Blood's PvP grab keeps the view on the victim. DL1's player grab sets the camera pitch to 0 on its
// first frame and leaves it there, so the drop attack shows the horizon. From the contact until the grab of
// type 7 / 8 ends, the plugin points the look pitch at the enemy's chest every frame, smoothed: eye = FPP
// model bone "head", target = enemy bone "spine2" (the enemy object is its own IModelObject, as in the AI
// code at FUN_1805EC4D0). Without bones: player position + 1.6 m / enemy position + 0.6 m.
// Look pitch (degrees, negative = down): player+0x1170 (aim, read by the melee code) and +0x1178 (camera,
// integrated from the mouse by FUN_180BA1350). Offsets read from the look setter FUN_180BA1C00.
typedef int    (__fastcall* FnBoneId)(void* model, const char* name);   // IModelObject::GetElementOrBoneElementID
typedef float* (__fastcall* FnBonePos)(void* model, float* out, int id); // IModelObject::GetElementWorldPos
static FnBoneId  s_boneId       = nullptr;
static FnBonePos s_bonePos      = nullptr;
static int32_t  s_pitchOff[2]   = {};
static char*    s_holdPlayer    = nullptr;  // attack while falling running: the camera follows s_holdTarget
static char*    s_holdTarget    = nullptr;
static int      s_holdBone      = -1;       // enemy chest bone, -1 = none
static void*    s_eyeModel      = nullptr;  // FPP model whose "head" bone is s_eyeBone
static int      s_eyeBone       = -1;
static float    s_holdPitch     = 0.0f;
static uint64_t s_holdStart     = 0;
static LARGE_INTEGER s_holdTick = {};
static bool     s_holdSeen      = false;    // the grab became active
static int32_t  s_holdPhase     = 0;        // its phase then (Begin, in the air)
static uint64_t s_holdLandAt    = 0;        // the phase changed (FinishWins): landed on the enemy
static int      s_holdLines     = 0;
static uint64_t s_holdLogAt     = 0;

static int FindBone(void* model, const char* const* names) {
    if (!s_boneId) return -1;
    for (; *names; names++) {
        int id = s_boneId(model, *names);
        if (id >= 0) return id;
    }
    return -1;
}

static float* PositionOf(char* obj, float* out) {
    return ((float*(__fastcall*)(void*, float*))(*(void***)obj)[0x270 / 8])(obj, out);
}

static bool Near(const float* a, const float* b, float d) {
    float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return x * x + y * y + z * z < d * d;
}

// Enemy chest; false = 0.6 m above its position (no bone, or the bone is far from the object).
static bool TargetPoint(float* out) {
    float pos[4];
    PositionOf(s_holdTarget, pos);
    if (s_holdBone >= 0) {
        s_bonePos(s_holdTarget, out, s_holdBone);
        if (Near(out, pos, 4.0f)) return true;
    }
    out[0] = pos[0]; out[1] = pos[1] + 0.6f; out[2] = pos[2];
    return false;
}

// Player's eye: the FPP model's "head" bone; false = 1.6 m above the player's position.
static bool EyePoint(void* animCtrl, char* player, float* out) {
    float pos[4];
    PositionOf(player, pos);
    char* holder = (char*)animCtrl + 0x40;
    void* model = ((void*(__fastcall*)(void*))(*(void***)holder)[1])(holder);
    if (model != s_eyeModel) {
        static const char* const kHead[] = { "head", nullptr };
        s_eyeModel = model;
        s_eyeBone = IsPtr(model) ? FindBone(model, kHead) : -1;
    }
    if (s_eyeBone >= 0) {
        s_bonePos(model, out, s_eyeBone);
        if (Near(out, pos, 3.0f)) return true;
    }
    out[0] = pos[0]; out[1] = pos[1] + 1.6f; out[2] = pos[2];
    return false;
}

static void HoldPitchStart(char* ctrl, char* target, uint64_t now) {
    __try {
        static const char* const kChest[] = { "spine2", "spine1", "spine", "pelvis", nullptr };
        char* player = (char*)((void*(__fastcall*)(void*))(*(void***)ctrl)[0x20 / 8])(ctrl);
        s_holdBone = FindBone(target, kChest);
        s_holdPitch = *(float*)(player + s_pitchOff[1]);
        s_holdTarget = target;
        s_holdPlayer = player;
        s_holdStart = now;
        s_holdLogAt = 0;
        s_holdSeen = false;
        s_holdLandAt = 0;
        QueryPerformanceCounter(&s_holdTick);
        if (s_holdLines < 80) {
            s_holdLines++;
            Log("  camera follows %p (chest bone %d), pitch %.0f at the contact", target, s_holdBone, s_holdPitch);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_holdPlayer = nullptr; }
}

// After AnimUpdate: aims the pitch at the enemy while the jump attack grab (type 7 / 8) runs, until
// JumpAttackCameraTime after the landing (the enemy is still sliding away from the kick).
static void HoldPitch(void* animCtrl) {
    __try {
        if (!IsFppCtrl(animCtrl)) return;
        char* player = *(char**)((char*)animCtrl + 0x50);
        if (player != s_holdPlayer) return;
        uint64_t now = GetTickCount64();
        int type = ((int(__fastcall*)(void*))(*(void***)player)[0x368 / 8])(player);
        char* grab = *(char**)(player + 0xBB8);
        bool active = grab[0x10] && (type == 7 || type == 8);
        if (active) {
            int32_t phase = *(int32_t*)(grab + 0xCC);
            if (!s_holdSeen) s_holdSeen = true, s_holdPhase = phase;
            else if (!s_holdLandAt && phase != s_holdPhase) s_holdLandAt = now;
        }
        bool given = s_holdLandAt && s_jumpCameraMs && now - s_holdLandAt >= s_jumpCameraMs;
        if ((!active && (s_holdSeen || now - s_holdStart > 500)) || now - s_holdStart > 6000 || given) {
            if (s_holdLines < 80) {
                s_holdLines++;
                Log("  +%llu ms: camera released at %.0f deg (%s)", (unsigned long long)(now - s_holdStart),
                    s_holdPitch, given ? "time after the landing" : "move over");
            }
            s_holdPlayer = nullptr;
            return;
        }
        float eye[4], tgt[4];
        bool eyeBone = EyePoint(animCtrl, player, eye);
        bool tgtBone = TargetPoint(tgt);
        float dx = tgt[0] - eye[0], dy = tgt[1] - eye[1], dz = tgt[2] - eye[2];
        float want = atan2f(dy, sqrtf(dx * dx + dz * dz)) * 57.29578f;
        if (want < -85.0f) want = -85.0f;
        if (want > 30.0f) want = 30.0f;
        LARGE_INTEGER tick, freq;
        QueryPerformanceCounter(&tick);
        QueryPerformanceFrequency(&freq);
        float dt = (float)(tick.QuadPart - s_holdTick.QuadPart) / (float)freq.QuadPart;
        s_holdTick = tick;
        if (dt > 0.1f) dt = 0.1f;
        s_holdPitch += (want - s_holdPitch) * (1.0f - expf(-dt * 10.0f));  // reaches the target in ~0.3 s
        float* cam = (float*)(player + s_pitchOff[1]);
        if (now >= s_holdLogAt && s_holdLines < 80) {
            s_holdLines++;
            s_holdLogAt = now + 250;
            Log("  +%llu ms: eye %.2f %.2f %.2f (%s), enemy %.2f %.2f %.2f (%s), game %.0f, camera %.0f -> %.0f",
                (unsigned long long)(now - s_holdStart), eye[0], eye[1], eye[2], eyeBone ? "head" : "position",
                tgt[0], tgt[1], tgt[2], tgtBone ? "chest" : "position", *cam, want, s_holdPitch);
        }
        *(float*)(player + s_pitchOff[0]) = s_holdPitch;
        *cam = s_holdPitch;
    } __except (EXCEPTION_EXECUTE_HANDLER) { s_holdPlayer = nullptr; }
}

// ── AnimUpdate hook ───────────────────────────────────────────────────────────
static uint64_t AnimUpdateShow(void* animCtrl) {
    if ((s_unlockChargeOk || s_unlockWhirlwindOk || s_unlockStompOk || s_unlockJumpAttackOk ||
         s_unlockGroundPoundOk) && IsFppCtrl(animCtrl))
        ApplyUnlocks(animCtrl);

    void* pending = s_pendingMgr;
    if (pending || s_showCtrl == animCtrl) {
        void* mgr = nullptr;
        __try { mgr = *(void**)((char*)animCtrl + 0x50); } __except (EXCEPTION_EXECUTE_HANDLER) {}

        // Wait until the new weapon is the active item: then the resolver sees its AnimPrefix and
        // the equip anim starts when the new weapon appears.
        if (pending && mgr == pending && IsFppCtrl(animCtrl)) {
            uint64_t now = GetTickCount64();
            if (now > s_pendingUntil) {
                s_pendingMgr = nullptr;
                s_instantMgr = nullptr;
                Log("  new weapon never became active, skipped");
            } else if (s_handItemSlot < 0) {
                s_pendingMgr = nullptr;
                ArmEquip(animCtrl);
            } else {
                void* item = ActiveItem(mgr);
                if (item && item != s_pendingItem) {
                    s_pendingMgr = nullptr;
                    Log("  active %p after %llu ms", item, (unsigned long long)(now - s_pendingStart));
                    if (s_hiddenAt)
                        // Bad Blood: no Show when the hands were busy (climb, vault, medkit...)
                        Log("  weapon was put away during the switch: no Show (like Bad Blood)");
                    else
                        ArmEquip(animCtrl);
                    // keep instant mode one more moment so the raise is skipped too
                    if (s_instantMgr == mgr) s_instantUntil = now + 100;
                }
            }
        }

        if (s_showCtrl == animCtrl && IsPtr(mgr)) {
            uint64_t now = GetTickCount64();
            int reason = 0;
            if (s_hiddenAt >= s_showArmedAt) reason = 1;
            else if (now < s_showUntil) {
                char* action = nullptr;
                __try { action = *(char**)((char*)mgr + s_actionOff); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                int32_t gameId = 0;
                if (IsPtr(action) && !GameActionActive(action, &gameId)) {
                    s_showFrames++;
                    s_showThisFrame = true;
                    if (!s_mainTrack) return s_origAnimUpdate(animCtrl);
                    ActionState saved;
                    BeginShow(action, &saved);
                    uint64_t ret = s_origAnimUpdate(animCtrl);
                    EndShow(action, &saved);
                    return ret;
                }
                if (s_showFrames == 0 && now - s_showArmedAt < 300) {
                    // a game action still runs (e.g. the old weapon's): start once it ends
                    s_showUntil = now + s_showMs;
                    s_showThisFrame = false;
                    if (!s_waitLogged) { Log("  game action 0x%X active, Show waits", gameId); s_waitLogged = true; }
                    return s_origAnimUpdate(animCtrl);
                }
                reason = s_showFrames ? 2 : 3;
                if (s_showFrames) Log("  game action 0x%X started", gameId);
            }
            StopShow(animCtrl, reason);
        }
    }
    return s_origAnimUpdate(animCtrl);
}

static uint64_t __fastcall hkAnimUpdate(void* animCtrl) {
    uint64_t ret = AnimUpdateShow(animCtrl);
    if (s_tracePlayer) TraceJump(animCtrl);
    if (s_holdPlayer) HoldPitch(animCtrl);
    return ret;
}

// ── Hand track hooks ──────────────────────────────────────────────────────────
// FUN_180BFF760 / FUN_180BFF450: copy the main track into hand track 3 / 2, then per-hand logic
// (carried objects, per-hand one-shots, hand weight). While the Show plays, finish like Bad Blood's
// FUN_180D7C8C0 does for its hand tracks: Show state, blend -1 (track default), speed 1, no loop.
static char* TrackOf(void* holder) { return IsPtr(holder) ? *(char**)((char*)holder + 0xD8) : nullptr; }

// MultislotAnimNode::NeedsUpdate (FUN_1810BCB60) re-applies the track when its stateId differs from
// the playing one (+0x24) or when the restart flag +0x1C is set; the apply (FUN_1810C32D0) clears it.
static void RestartNode(void* holder) { if (IsPtr(holder)) *((char*)holder + 0x1C) = 1; }

static void ApplyHandShow(void* animCtrl, int holderOff, const char* which) {
    __try {
        auto mainHolder = ((void*(__fastcall*)(void*))(*(void***)animCtrl)[0x8F0 / 8])(animCtrl);
        char* main  = TrackOf(mainHolder);
        char* holders = *(char**)((char*)animCtrl + 0x1A8);
        void* handHolder = IsPtr(holders) ? *(void**)(holders + holderOff) : nullptr;
        char* hand  = TrackOf(handHolder);
        if (!IsPtr(main) || !IsPtr(hand)) return;
        int32_t vanilla = *(int32_t*)(hand + 0x10), mainId = *(int32_t*)(main + 0x10);
        if (vanilla != mainId && vanilla != s_showId) {
            if (s_handFixes++ == 0)
                Log("  %s hand track had its own state 0x%X (main 0x%X), forced to Show", which, vanilla,
                    mainId);
        }
        *(uint64_t*)(hand + 0x08) = *(uint64_t*)(main + 0x08);
        memcpy(hand + 0x10, main + 0x10, 0x1C);  // +0x10 .. +0x2B
        hand[0x2C] = main[0x2C];
        hand[0x2E] = main[0x2E];
        hand[0x2D] = 1;
        *(int32_t*)(hand + 0x10) = s_showId;
        *(float*)(hand + 0x20)   = -1.0f;
        *(float*)(hand + 0x1C)   = 1.0f;
        hand[0x2C] = 0;
        if (s_showFrames == 1) {
            RestartNode(handHolder);
            if (s_mainTrack && holderOff == 0x18) RestartNode(mainHolder);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall hkHandOff(void* animCtrl) {
    s_origHandOff(animCtrl);
    if (s_showThisFrame && s_showCtrl == animCtrl) ApplyHandShow(animCtrl, 0x18, "off-hand (1)");
}

static void __fastcall hkHandMain(void* animCtrl) {
    s_origHandMain(animCtrl);
    if (s_showThisFrame && s_showCtrl == animCtrl) ApplyHandShow(animCtrl, 0x10, "main-hand (0)");
}

// ── Weapon input hooks ────────────────────────────────────────────────────────
// Bad Blood's weapon controllers return from OnInput while the weapon is shown or hidden
// (weaponMgr+0x1CB8/+0x1CB9, e.g. BB FUN_180C77EA0), so no attack or aim starts during the Show.
// DL1 has the same OnInput(ctrl, action, value, flag) at vtable 0xB0; the owner is vtable[0x20/8].
// Releases (value 0) and the movement actions BB lets through (0-3, 0x32-0x35) still pass.
static bool ShowBlocksInput(void* ctrl, uint32_t action, float value) {
    if (!s_showCtrl || !s_showFrames || value == 0.0f || action < 4 || action - 0x32 < 4) return false;
    if (GetTickCount64() >= s_showUntil) return false;
    void* owner = nullptr;
    __try { owner = ((void*(__fastcall*)(void*))(*(void***)ctrl)[0x20 / 8])(ctrl); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (owner != s_switchMgr) return false;
    if (s_inputBlocked++ == 0) Log("  weapon input 0x%X ignored during the Show", action);
    return true;
}

// ── Attack while falling ──────────────────────────────────────────────────────
// Same code in both games: the move starts when the player touches an enemy (FUN_180DCBC50, from the
// collision callback) while the gate FUN_180DC4900 passes. The gate wants a charge stance (+0x328) or an
// attack started in the air with the button still down (+0x770, action 0x45). A click released before the
// contact fails it, so with JumpAttackClick a press in the air counts as a charge stance until landing.
typedef char  (__fastcall* FnJumpCheck)(void* ctrl);
typedef char  (__fastcall* FnJumpTarget)(void* ctrl, void* target);
typedef void  (__fastcall* FnJumpHit)(void* ctrl, void* target, void* a3, void* a4);
typedef int   (__fastcall* FnJumpWeapon)(void* ctrl);
typedef float (__fastcall* FnActionValue)(void* ctrl, int action);

static FnJumpCheck   s_origJumpGate   = nullptr;  // FUN_180DC4900
static FnJumpTarget  s_origJumpTarget = nullptr;  // FUN_180DC6B30
static FnJumpHit     s_origJumpHit    = nullptr;  // FUN_180DCBC50
static FnJumpCheck   s_jumpAir        = nullptr;  // FUN_180DC64F0: skill, in the air for 0.2 s
static FnJumpWeapon  s_jumpWeapon     = nullptr;  // FUN_180DCCAA0: 7 cutting, 8 blunt, -1 not allowed
static FnActionValue s_actionValue    = nullptr;  // FUN_180D5EB70
static char*    s_airPressCtrl  = nullptr;
static uint64_t s_airPressTime  = 0;
static uint64_t s_lastContact   = 0;
static int      s_lastGate      = -1;
static void*    s_lastTarget    = nullptr;
static int      s_lastTargetOk  = -1;
static int      s_targetLines   = 0;
static int      s_jumpLogLines  = 0;

static void LogJumpState(char* ctrl, const char* what) {
    if (!s_jumpAttackLog || s_jumpLogLines >= 300) return;
    s_jumpLogLines++;
    __try {
        char* player = (char*)((void*(__fastcall*)(void*))(*(void***)ctrl)[0x20 / 8])(ctrl);
        float buf[16];
        float* pos = ((float*(__fastcall*)(void*, float*))(*(void***)player)[0x270 / 8])(player, buf);
        Log("  %s: air+skill %d, weapon %d, stance %d, air attack %d, hold %.2f, state %d, next %d, hand %d, "
            "cooldown %.2f, busy %d, y %.2f, fall start %.2f", what, s_jumpAir(ctrl) != 0, s_jumpWeapon(ctrl),
            *(int32_t*)(ctrl + 0x328), ctrl[0x770] != 0, s_actionValue(ctrl, 0x45), *(int32_t*)(ctrl + 0x110),
            *(int32_t*)(ctrl + 0x11C), *(int32_t*)(*(char**)(player + 0xC58) + 0x38),
            *(float*)(*(char**)(player + 0x1380) + 0x5C), *(*(char**)(player + 0xBB8) + 0x10) != 0,
            pos[1], *(float*)(player + 0x1228));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static char __fastcall hkJumpGate(char* ctrl) {
    bool pressed = s_airPressCtrl == ctrl;
    int32_t* stance = (int32_t*)(ctrl + 0x328);
    int32_t saved = *stance;
    if (pressed && saved == 0) *stance = 1;
    char passed = s_origJumpGate(ctrl);
    *stance = saved;
    if (pressed && !passed && GetTickCount64() - s_airPressTime > 300 && !s_jumpAir(ctrl)) {
        s_airPressCtrl = nullptr;
        pressed = false;
        LogJumpState(ctrl, "landed");
    }
    if ((pressed || ctrl[0x770]) && (passed != 0) != (s_lastGate == 1)) {
        s_lastGate = passed != 0;
        LogJumpState(ctrl, passed ? "gate open" : "gate closed");
    }
    return passed;
}

static char __fastcall hkJumpTarget(char* ctrl, char* target) {
    char ok = s_origJumpTarget(ctrl, target);
    if (s_jumpAttackLog && target && (target != s_lastTarget || (ok != 0) != (s_lastTargetOk == 1)) &&
        (s_airPressCtrl == ctrl || ctrl[0x770]) && s_jumpLogLines < 300 && s_targetLines < 6) {
        s_lastTarget = target;
        s_lastTargetOk = ok != 0;
        s_targetLines++;
        s_jumpLogLines++;
        __try {
            float buf[16];
            float* pos = ((float*(__fastcall*)(void*, float*))(*(void***)target)[0x270 / 8])(target, buf);
            Log("  target %p: %s, its y %.2f", target, ok ? "valid" : "rejected", pos[1]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return ok;
}

static void __fastcall hkJumpHit(char* ctrl, void* target, void* a3, void* a4) {
    uint64_t now = GetTickCount64();
    bool log = s_jumpAttackLog && now - s_lastContact > 250;
    bool hold = s_pitchOff[0] && !s_holdPlayer;
    if ((log || hold) && s_jumpAir(ctrl)) {
        char passed = hkJumpGate(ctrl);
        char ok = s_origJumpTarget(ctrl, target);
        if (log) {
            s_lastContact = now;
            if (s_jumpLogLines < 300) {
                s_jumpLogLines++;
                Log("Contact in the air with %p: gate %d, target %d%s", target, passed != 0, ok != 0,
                    passed && ok ? ", the move starts" : "");
            }
            if (passed && ok && s_traceLines < 400) {
                __try { TraceStart((char*)((void*(__fastcall*)(void*))(*(void***)ctrl)[0x20 / 8])(ctrl)); }
                __except (EXCEPTION_EXECUTE_HANDLER) { s_tracePlayer = nullptr; }
            }
        }
        if (passed && ok && hold) HoldPitchStart(ctrl, (char*)target, now);
    }
    s_origJumpHit(ctrl, target, a3, a4);
}

// GrabController anim events (vtable 0xC0, FUN_180D26750): GRAB_DESTROY_HEAD (622) only acts while the
// controller's byte +0xDC is set. Its update FUN_180D24280 clears it for grab types 7-16 unless a player value
// (vtable 0xA20, id 128) reaches 0.7, so the attack while falling (types 7 / 8) gets it back for that event.
typedef void (__fastcall* FnGrabEvent)(char* grab, char* ev);
static FnGrabEvent s_origGrabEvent = nullptr;
static int32_t     s_headFlagOff   = 0;
static int         s_headLines     = 0;

static void __fastcall hkGrabEvent(char* grab, char* ev) {
    __try {
        if (*(int32_t*)ev == 3 && *(uint16_t*)(*(char**)(ev + 0x18) + 2) == 622) {
            char* player = (char*)((void*(__fastcall*)(void*))(*(void***)grab)[0x20 / 8])(grab);
            int type = ((int(__fastcall*)(void*))(*(void***)player)[0x368 / 8])(player);
            if (type == 7 || type == 8) {
                if (s_headLines < 20) {
                    s_headLines++;
                    Log("  +%llu ms: head destroyed at the impact (flag was %d)",
                        (unsigned long long)(GetTickCount64() - s_holdStart), grab[s_headFlagOff]);
                }
                grab[s_headFlagOff] = 1;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    s_origGrabEvent(grab, ev);
}

static void __fastcall hkFireInput(void* ctrl, uint32_t action, float value, char flag) {
    if (!ShowBlocksInput(ctrl, action, value)) s_origFireInput(ctrl, action, value, flag);
}
static void __fastcall hkMeleeInput(void* ctrl, uint32_t action, float value, char flag) {
    if (s_blockAttacks && ShowBlocksInput(ctrl, action, value)) return;
    s_origMeleeInput(ctrl, action, value, flag);
    if (action == 0x9F && value == 1.0f && s_jumpAir && s_jumpAir(ctrl)) {  // _ACTION_ATTACK_TRIGGER_R
        uint64_t now = GetTickCount64();
        if (now - s_airPressTime > 500) {
            s_lastGate = -1;
            s_lastTarget = nullptr;
            s_targetLines = 0;
            LogJumpState((char*)ctrl, "attack pressed in the air");
        }
        s_airPressTime = now;
        if (s_jumpAttackClick && s_origJumpGate) s_airPressCtrl = (char*)ctrl;
    }
}
static void __fastcall hkBowInput(void* ctrl, uint32_t action, float value, char flag) {
    if (!ShowBlocksInput(ctrl, action, value)) s_origBowInput(ctrl, action, value, flag);
}

// ── BlendCompute hook ─────────────────────────────────────────────────────────
// FUN_180BAEEC0(weaponMgr, hand, instant): per-frame hand switch. instant=1 makes the lower and
// raise durations 0, so the requested weapon becomes active in the same frame.
// It also picks the item the hand brings up (target node +0x1670 + hand*0x20). An empty target while
// the switched-to weapon is requested means the hands are busy (climbing, vaulting, medkit...):
// the weapon is put away, the same condition that makes Bad Blood switch without its Show.
static uint64_t __fastcall hkBlendCompute(void* weaponMgr, int hand, char instant) {
    if (weaponMgr == s_instantMgr) {
        if (GetTickCount64() < s_instantUntil) instant = 1;
        else s_instantMgr = nullptr;
    }
    uint64_t ret = s_origBlendCompute(weaponMgr, hand, instant);
    if (s_targetNodesOk && s_requestedItem && hand == s_requestedHand && weaponMgr == s_switchMgr &&
        (s_pendingMgr || s_showCtrl)) {
        __try {
            char* m = (char*)weaponMgr + hand * 0x20;
            void* requested = *(void**)(m + 0x16B0) ? *(void**)(m + 0x16B8) : nullptr;
            if (requested == s_requestedItem && !*(void**)(m + 0x1670) && !s_hiddenAt) {
                s_hiddenAt = GetTickCount64();
                Log("  weapon put away (hand %d busy)", hand);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return ret;
}

// ── Init ──────────────────────────────────────────────────────────────────────
struct Sig { const char* name; const char* pattern; uintptr_t* out; bool required; };

static bool ResolveGame() {
    uintptr_t slot = 0, anim = 0, resolve = 0, blend = 0, prefix = 0, durWrite = 0, sentinelRef = 0;
    Sig sigs[] = {
        { "SetWeaponSlot",
          "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 54 41 56 41 57 48 83 EC 20 "
          "45 0F B6 F8 8B EA 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 48 58 48 8B 40 58 FF 90 C0 01 00 00",
          &slot, true },
        { "AnimUpdate",
          "48 89 5C 24 20 55 56 57 48 8B EC 48 83 EC 50 48 8B 01 48 8B F1 FF 90 F0 08 00 00 "
          "48 8B B8 D8 00 00 00 48 8B CF 48 8B 07",
          &anim, true },
        { "ResolveStateId",
          "40 55 56 57 41 57 48 83 EC 48 49 8B E9 4D 8B F8 48 8B FA 48 8B F1 4D 85 C9 0F 84 ?? ?? ?? ?? "
          "49 8B C9 FF 15 ?? ?? ?? ??",
          &resolve, true },
        // landing anim: call [rax+0xD38] ; movss [rbx+0x1830], xmm0  (checks both offsets)
        { "DurationWrite", "FF 90 38 0D 00 00 F3 0F 11 83 30 18 00 00", &durWrite, true },
        // one-shot consumer: mov rax,[rip+x] ; mov ecx,[rsi+0x182C] ; cmp ecx,[rax]
        { "SentinelRef", "48 8B 05 ?? ?? ?? ?? 8B 8E 2C 18 00 00 3B 08", &sentinelRef, true },
        // also checks the 0.2 s default lower time load and the instant test (r8b)
        { "BlendCompute",
          "48 8B C4 44 88 40 18 89 50 10 48 89 48 08 55 57 41 54 48 8D A8 18 FF FF FF 48 81 EC D0 01 00 00 "
          "0F 29 78 98 48 8B F9 44 0F 29 88 78 FF FF FF 0F 57 FF 44 0F 29 98 58 FF FF FF "
          "F3 44 0F 10 1D ?? ?? ?? ?? 4C 63 E2 45 84 C0 74 06 45 0F 57 C9 EB 04 45 0F 28 CB 80 79 50 00",
          &blend, false },
        { "GetAnimPrefix",
          "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B 59 50 8B FA 48 8B E9 83 FA FF 75",
          &prefix, false },
    };
    bool ok = true;
    for (auto& s : sigs) {
        *s.out = Scan(s.pattern);
        if (*s.out) Log("  %-15s RVA 0x%llX", s.name, (unsigned long long)(*s.out - s_base));
        else Log("  %-15s %s", s.name, s.required ? "NOT FOUND" : "not found (optional)");
        if (!*s.out && s.required) ok = false;
    }
    if (!ok) return false;

    // FUN_180BF8170: lea rsi,[rip+"Fpp_"] ; strlen ; append. Its Tpp_ twin differs only by the string.
    uintptr_t fppStr = FindString("Fpp_");
    if (fppStr)
        s_fppPrefixFn = (void*)Scan("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B FA 48 8D 35 ?? ?? ?? ?? "
                                    "48 C7 C3 FF FF FF FF", 0x15, fppStr);
    if (s_fppPrefixFn) Log("  FppPrefix       RVA 0x%llX", (unsigned long long)((uintptr_t)s_fppPrefixFn - s_base));
    else Log("  FppPrefix       not found (optional) - controller filter off");

    s_sentinelPtr = (int32_t**)RipTarget(sentinelRef, 3, 7);
    __try {
        if (!IsPtr(*s_sentinelPtr)) { Log("ERROR: sentinel pointer invalid"); return false; }
        Log("  sentinel        0x%X", **s_sentinelPtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) { Log("ERROR: sentinel unreadable"); return false; }

    s_resolve   = (FnResolveStateId)resolve;
    s_getPrefix = (FnGetAnimPrefix)prefix;

    // AnimUpdate's first branch: mov rdx,[rax+0xBF8] ; cmp byte [rdx+0x10],0 ; jz ;
    // movsxd rax,[rdx+0x38] ; lea rcx,[rax+rax*2] ; mov eax,[rdx+rcx*4+0x44] ; mov [rdi+0x10],eax
    uintptr_t actionBranch = Scan("48 8B 90 ?? ?? 00 00 80 7A 10 00 74 ?? 48 63 42 38 48 8D 0C 40 8B 44 8A 44 89 47 10",
                                  -1, 0, anim, anim + 0x120);
    if (!actionBranch) { Log("ERROR: AnimUpdate action-state branch not found"); return false; }
    s_actionOff = *(int32_t*)(actionBranch + 3);
    Log("  ActionState     weaponMgr+0x%X", s_actionOff);

    // GetAnimPrefix+0x2F: call [rax+imm32] — weaponMgr's active item getter;
    // GetAnimPrefix+0xC6: call [rax+imm32] on item+0x58 — the item's AnimPrefix getter
    if (prefix && *(uint16_t*)(prefix + 0x2F) == 0x90FF) {
        s_handItemSlot = *(int32_t*)(prefix + 0x31) / 8;
        Log("  HandItem        vtable+0x%X", s_handItemSlot * 8);
    } else {
        Log("  HandItem        not found - equip anim fires at switch start");
    }
    if (prefix && *(uint16_t*)(prefix + 0xC6) == 0x90FF) {
        s_itemPrefixSlot = *(int32_t*)(prefix + 0xC8) / 8;
        Log("  ItemPrefix      vtable+0x%X", s_itemPrefixSlot * 8);
    }
    // SetWeaponSlot: lea rcx,[rbx+0x16B0] and cmp [rbx+0x16D0],r12 (requested hand nodes)
    s_requestedNodesOk = CodeHas(slot, 0x2B0, "\x48\x8D\x8B\xB0\x16\x00\x00", 7) &&
                         CodeHas(slot, 0x2B0, "\x4C\x39\xA3\xD0\x16\x00\x00", 7);
    // BlendCompute: lea rcx,[r14+0x1670] ; call (sets the hand's target node, r14 = mgr + hand*0x20)
    s_targetNodesOk = blend && s_requestedNodesOk &&
                      CodeHas(blend, 0x800, "\x49\x8D\x8E\x70\x16\x00\x00\xE8", 8);
    s_canInstant = s_instantSwitch && blend && s_requestedNodesOk && s_itemPrefixSlot >= 0;
    Log("  InstantSwitch   %s", s_canInstant ? "on" : (s_instantSwitch ? "unavailable" : "off"));
    Log("  BusyCheck       %s", s_targetNodesOk ? "on (target nodes +0x1670)" : "unavailable");

    // FPP vis hand tracks (vtable 0xAE8 / 0xAF0): mov rax,[rcx+0x1A8] ; mov rdx,[rax+0x18/0x10] ;
    // mov reg,[rdx+0xD8] — the offsets ApplyHandShow uses are part of the patterns
    uintptr_t handOff = 0, handMain = 0;
    if (s_handTracks) {
        handOff  = Scan("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 40 48 8B 81 A8 01 00 00 "
                        "4C 8B F1 0F 29 74 24 30 0F 29 7C 24 20 48 8B 50 18 48 8B BA D8 00 00 00");
        handMain = Scan("48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 70 48 8B 81 A8 01 00 00 "
                        "48 8B F9 0F 29 74 24 60 0F 29 7C 24 50 44 0F 29 44 24 40 48 8B 50 10 48 8B B2 D8 00 00 00");
        if (handOff && handMain)
            Log("  HandTracks      RVA 0x%llX / 0x%llX", (unsigned long long)(handOff - s_base),
                (unsigned long long)(handMain - s_base));
        else
            Log("  HandTracks      not found (optional)");
    } else {
        Log("  HandTracks      off");
    }
    if (!handOff || !handMain) {
        handOff = handMain = 0;
        s_mainTrack = true;  // without hand tracks the Show can only play on the main track
    }
    Log("  MainTrack       %s", s_mainTrack ? "on (Show also on the main track)" : "off (like Bad Blood)");

    // weapon controllers' OnInput (vtable 0xB0)
    uintptr_t fireIn = 0, meleeIn = 0, bowIn = 0;
    if (s_blockAttacks) {
        fireIn  = Scan("48 89 5C 24 10 48 89 6C 24 18 57 48 83 EC 30 80 79 10 00 41 0F B6 E9 0F 29 74 24 20 "
                       "8B FA 0F 28 F2 48 8B D9 0F 84");
        bowIn   = Scan("45 84 C9 0F 85 ?? ?? ?? ?? 48 89 5C 24 08 57 48 83 EC 30 8B FA 0F 29 74 24 20 "
                       "0F 28 F2 48 8B D9 44 38 49 10 0F 84");
    }
    if (s_blockAttacks || s_jumpAttackClick || s_jumpAttackLog)
        meleeIn = Scan("48 89 5C 24 10 57 48 83 EC 30 80 79 10 00 8B DA 0F 29 74 24 20 48 8B F9 0F 28 F2 "
                       "75 58 81 FA 9F 00 00 00 74 08 81 FA D4");
    if (s_blockAttacks) {
        Log("  BlockAttacks    fire %s, melee %s, bow %s", fireIn ? "on" : "not found",
            meleeIn ? "on" : "not found", bowIn ? "on" : "not found");
    } else {
        Log("  BlockAttacks    off");
    }

    // layout of the skill properties, checked on the code that reads them
    uintptr_t stompGetter = Scan("40 53 48 83 EC 20 48 8B 01 FF 50 20 48 8B D8 80 78 50 00 74 17 48 8B 10 48 8B C8 "
                                 "FF 92 C8 09 00 00 84 C0 48 8B 05 ?? ?? ?? ?? 74 07 48 8B 83 58 0E 00 00 "
                                 "48 8B 80 D0 00 00 00 48 8B 08 48 8B 81 08 06 00 00");
    uintptr_t chargeGate  = Scan("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 30 48 8B 01 48 8B F1 "
                                 "FF 50 20 48 8B D8 80 78 50 00 74 17 48");
    bool gateOk = chargeGate && CodeHas(chargeGate, 0x150, "\x28\x1D\x00\x00", 4) &&
                  CodeHas(chargeGate, 0x150, "\x20\x1D\x00\x00", 4);
    uintptr_t jumpGate = Scan("40 53 48 83 EC 40 48 8B 01 48 8B D9 FF 50 20 48 8D 48 18 FF 15 ?? ?? ?? ?? "
                              "48 8B C8 48 8B 10 FF 92 B0 00 00 00 48 85 C0 0F 84");
    uintptr_t poundGate = Scan("0F 57 F6 48 89 74 24 40 48 8B 08 48 8B 81 00 11 00 00 0F 2F 70 08 0F 83");
    if (stompGetter)  // mov rax,[rip+disp] at +35, next instruction at +42
        s_propsGlobal = (char**)(stompGetter + 42 + *(int32_t*)(stompGetter + 38));
    s_unlockChargeOk      = s_unlockCharge && gateOk && stompGetter;
    s_unlockWhirlwindOk   = s_unlockWhirlwind && gateOk && stompGetter;
    s_unlockStompOk       = s_unlockStomp && stompGetter;
    s_unlockJumpAttackOk  = s_unlockJumpAttack && stompGetter && jumpGate &&
                            CodeHas(jumpGate, 0x100, "\x48\x8B\x81\x00\x1E\x00\x00\xF6\x40\x08\x01", 11);
    s_unlockGroundPoundOk = s_unlockGroundPound && stompGetter && poundGate &&
                            CodeHas(poundGate, 0x140, "\xBA\x25\x02\x00\x00", 5);
    Log("  Unlocks         charge %s, whirlwind %s, stomp %s, jump attack %s, ground pound %s",
        s_unlockChargeOk ? "on" : (s_unlockCharge ? "not found" : "off"),
        s_unlockWhirlwindOk ? "on" : (s_unlockWhirlwind ? "not found" : "off"),
        s_unlockStompOk ? "on" : (s_unlockStomp ? "not found" : "off"),
        s_unlockJumpAttackOk ? "on" : (s_unlockJumpAttack ? "not found" : "off"),
        s_unlockGroundPoundOk ? "on" : (s_unlockGroundPound ? "not found" : "off"));

    uintptr_t jumpCheck = 0, jumpTarget = 0, jumpHit = 0;
    if (s_jumpAttackClick || s_jumpAttackLog || s_jumpCamera) {
        jumpCheck  = Scan("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 48 8B 01 48 8B D9 0F 29 74 24 20 FF 50 20 "
                          "33 D2 48 8B C8 4C 8B 00 41 FF 90");
        jumpTarget = Scan("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 0F 29 74 24 50 48 8B DA 0F 29 7C 24 40 "
                          "48 8B F1 44 0F 29 44 24 30 48 85 D2");
        jumpHit    = Scan("48 89 5C 24 10 48 89 74 24 18 4C 89 74 24 20 55 48 8D 6C 24 D0 48 81 EC 30 01 00 00 "
                          "48 8B 01 4C 8B F2 48 8B F1 FF 50 20");
        uintptr_t weapon = Scan("48 89 5C 24 10 48 89 6C 24 18 56 48 83 EC 20 48 8B F1 BD FF FF FF FF E8 ?? ?? ?? ?? "
                                "48 8B 16 48 8B CE 8B D8 FF 52 20 8B");
        uintptr_t action = Scan("48 89 5C 24 08 57 48 83 EC 20 48 8B 01 8B FA 48 8B D9 FF 50 20 48 85 C0 74 22 "
                                "48 8B 03 48 8B CB FF 50 20 8B D7 48 8B C8");
        if (jumpGate && jumpCheck && jumpTarget && jumpHit && weapon && action && meleeIn) {
            s_jumpAir     = (FnJumpCheck)jumpGate;
            s_jumpWeapon  = (FnJumpWeapon)weapon;
            s_actionValue = (FnActionValue)action;
        } else {
            jumpCheck = jumpTarget = jumpHit = 0;
        }
        // look setter FUN_180BA1C00(player, dir, notify): movss [rbp+aim pitch] at +0x4D, [rbp+camera pitch] at +0x77
        uintptr_t look = s_jumpCamera ? Scan("48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 30 F3 0F 10 0A "
                                             "41 0F B6 F8 F3 0F 10 42 08 48 8B DA 0F 29 74 24") : 0;
        if (jumpCheck && look && CodeHas(look + 0x4D, 4, "\xF3\x0F\x11\x85", 4) &&
            CodeHas(look + 0x77, 4, "\xF3\x0F\x11\x85", 4)) {
            int32_t aim = *(int32_t*)(look + 0x51), cam = *(int32_t*)(look + 0x7B);
            if (aim > 0 && aim < 0x4000 && cam > 0 && cam < 0x4000) {
                s_pitchOff[0] = aim;
                s_pitchOff[1] = cam;
            }
        }
        HMODULE engine = GetModuleHandleA("engine_x64_rwdi.dll");
        if (s_pitchOff[0] && engine) {
            s_boneId  = (FnBoneId)GetProcAddress(engine, "?GetElementOrBoneElementID@IModelObject@@QEBAHPEBD@Z");
            s_bonePos = (FnBonePos)GetProcAddress(engine, "?GetElementWorldPos@IModelObject@@QEBA?AVvec3@@H@Z");
            if (!s_boneId || !s_bonePos) s_boneId = nullptr, s_bonePos = nullptr;
        }
        char camera[64];
        if (s_pitchOff[0]) snprintf(camera, sizeof(camera), "follows the enemy (pitch +0x%X/+0x%X, bones %s)",
                                    s_pitchOff[0], s_pitchOff[1], s_boneId ? "on" : "not found");
        else snprintf(camera, sizeof(camera), "%s", s_jumpCamera ? "not found" : "off");
        Log("  JumpAttack      click %s, log %s, camera %s", jumpCheck && s_jumpAttackClick ? "on" : "off",
            jumpCheck && s_jumpAttackLog ? "on" : "off", camera);
    }

    // GrabController anim events: cmp ax==611 / 622 (mov ecx,0x26E at +0x34), cmp byte [rbx+flag],0 at +0x3F
    uintptr_t grabEvent = s_jumpHead ? Scan("48 89 5C 24 08 57 48 83 EC 20 83 3A 03 48 8B FA 48 8B D9 0F 85 ?? ?? ?? ?? "
                                            "48 8B 42 18 B9 63 02 00 00 66 39 48 02 75 0C") : 0;
    if (grabEvent && CodeHas(grabEvent + 0x34, 5, "\xB9\x6E\x02\x00\x00", 5) && CodeHas(grabEvent + 0x3F, 2, "\x80\xBB", 2)) {
        int32_t off = *(int32_t*)(grabEvent + 0x41);
        if (off > 0 && off < 0x1000) s_headFlagOff = off;
    }
    if (!s_headFlagOff) grabEvent = 0;
    if (s_jumpHead) Log("  JumpAttackHead  %s", grabEvent ? "on" : "not found");

    if (MH_Initialize() != MH_OK) { Log("ERROR: MH_Initialize"); return false; }
    if (MH_CreateHook((void*)slot, (void*)hkSetWeaponSlot, (void**)&s_origSlot) != MH_OK ||
        MH_CreateHook((void*)anim, (void*)hkAnimUpdate,    (void**)&s_origAnimUpdate) != MH_OK) {
        Log("ERROR: MH_CreateHook"); return false;
    }
    if ((s_canInstant || s_targetNodesOk) &&
        MH_CreateHook((void*)blend, (void*)hkBlendCompute, (void**)&s_origBlendCompute) != MH_OK) {
        Log("WARN: BlendCompute hook failed, InstantSwitch and busy check disabled");
        s_origBlendCompute = nullptr;
        s_canInstant = s_targetNodesOk = false;
    }
    if (handOff && handMain &&
        (MH_CreateHook((void*)handOff,  (void*)hkHandOff,  (void**)&s_origHandOff)  != MH_OK ||
         MH_CreateHook((void*)handMain, (void*)hkHandMain, (void**)&s_origHandMain) != MH_OK)) {
        Log("WARN: hand track hooks failed, Show on the main track");
        MH_RemoveHook((void*)handOff);
        MH_RemoveHook((void*)handMain);
        s_mainTrack = true;
    }
    if (fireIn  && MH_CreateHook((void*)fireIn,  (void*)hkFireInput,  (void**)&s_origFireInput)  != MH_OK)
        Log("WARN: fire input hook failed");
    if (meleeIn && MH_CreateHook((void*)meleeIn, (void*)hkMeleeInput, (void**)&s_origMeleeInput) != MH_OK)
        Log("WARN: melee input hook failed");
    if (bowIn   && MH_CreateHook((void*)bowIn,   (void*)hkBowInput,   (void**)&s_origBowInput)   != MH_OK)
        Log("WARN: bow input hook failed");
    if (jumpCheck && s_origMeleeInput &&
        (MH_CreateHook((void*)jumpCheck,  (void*)hkJumpGate,   (void**)&s_origJumpGate)   != MH_OK ||
         MH_CreateHook((void*)jumpTarget, (void*)hkJumpTarget, (void**)&s_origJumpTarget) != MH_OK ||
         MH_CreateHook((void*)jumpHit,    (void*)hkJumpHit,    (void**)&s_origJumpHit)    != MH_OK)) {
        Log("WARN: jump attack hooks failed");
        MH_RemoveHook((void*)jumpCheck);
        MH_RemoveHook((void*)jumpTarget);
        MH_RemoveHook((void*)jumpHit);
        s_origJumpGate = nullptr;
        s_jumpAir = nullptr;
    }
    if (grabEvent && MH_CreateHook((void*)grabEvent, (void*)hkGrabEvent, (void**)&s_origGrabEvent) != MH_OK) {
        Log("WARN: grab event hook failed, the head stays on");
        s_origGrabEvent = nullptr;
    }
    if (MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Log("ERROR: MH_EnableHook"); return false; }
    return true;
}

static DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 600 && !s_base; i++) {
        s_base = (uintptr_t)GetModuleHandleW(L"gamedll_x64_rwdi.dll");
        if (!s_base) Sleep(100);
    }
    if (!s_base) { Log("ERROR: gamedll not found"); return 0; }
    Log("gamedll base: %p  anim='%s' instantSwitch=%d", (void*)s_base, s_animName, (int)s_instantSwitch);
    if (!s_enabled) { Log("disabled in equip_anim.ini"); return 0; }
    if (!FindSections(s_base)) { Log("ERROR: .text/.rdata not found"); return 0; }

    if (ResolveGame()) Log("Hooks enabled");
    else Log("ERROR: game version not supported, plugin inactive");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        s_self = hInst;
        DisableThreadLibraryCalls(hInst);
        LoadConfig();
        FILE* f = fopen(s_logPath, "w");  // fresh log per session
        if (f) fclose(f);
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
    }
    return TRUE;
}
