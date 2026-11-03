# Dying Light 1 — Weapon Equip Animation Port (Bad Blood → DL1)

*Last updated: 2026-10-29 — game build 24076292 (Steam update of 2026-07-15, gamedll timestamp 2026-07-05)*

---

## 1. Goal

Play the Dying Light: Bad Blood ("dlbb", `_dlbg` suffix) weapon equip animations in Dying Light 1 every time
the player switches weapon. Both games run Chrome Engine 6 and share the `.scr` / `.anm2` / rpack formats.

Two parts are needed:

| Part | What | Where |
|------|------|-------|
| Data | `fpp_<AnimPrefix>_show` tracks in the compiled player animation banks + the 8 `_dlbg.anm2` files | `DW\Data\common_anims_PC.rpack` |
| Code | trigger the Show state on weapon switch (DL1 has no code for it) | `equip_anim.asi` |

---

## 2. Install / uninstall

```
python tools\build_equip_rpack.py ^
    --game "C:\Program Files (x86)\Steam\steamapps\common\Dying Light" ^
    --anims "C:\...\dlbb\unpack_dlbb\Animation" --install
```
then copy to the game folder (next to `dinput8.dll` = Ultimate ASI Loader, and `MinHook.x64.dll`):
- `equip_anim\equip_anim\equip_anim\x64\Release\equip_anim.asi`
- `equip_anim\equip_anim.ini`

The builder keeps `common_anims_PC.rpack.equip_anim_backup` (vanilla) on first run and always builds from it.
**Uninstall:** delete `equip_anim.asi`, copy the backup over `common_anims_PC.rpack` (or Steam → Verify integrity).
**After a game update:** Steam replaces the rpack → delete the old backup, re-run the builder. The plugin finds
its addresses by signature; if one is missing it logs `game version not supported` and does nothing.

Build the plugin: `MSBuild plugin\equip_anim.vcxproj -p:Configuration=Release -p:Platform=x64`
(source: `plugin\dllmain.cpp`, links `MinHook.x64.lib`, static CRT).

---

## 3. Why it never worked before (fixed)

1. **The rpack never contained the Show tracks.** The old repacker (`tools\...\RP6_RepackCLI`) only rebuilt the
   Animation section and copied the compressed AnimationScr sections verbatim, so the patched
   `anims_player_part00.bin` was silently dropped (the deployed rpack had the vanilla 580 934-byte bank).
   Every lookup of a Show state returned the sentinel.
2. **The bank patch itself was corrupt.** `AnimBinPatcher` "shifted record indices" in the 66 KB block after the
   records. That block is the event list (see §4): it rewrote event times and callback indices of hundreds of
   vanilla animations. Only `anims_player` was patched, not `anims_player_man_all`.
3. **The DevTools build (2026-03-04) is older than the retail data**: its `anims_player` lacks 132 retail
   tracks (chainsaw, DLC weapons…) — deploying it breaks those weapons.
4. **The 2026-07-15 game update** moved all gamedll code (−0x690 in the anim/weapon region) and replaced the
   rpack. The hard-coded RVAs of the old plugin pointed into the middle of instructions.
5. The old `SetWeaponSlot` hook was declared `void`; the real function returns `bool` (AL), so the game got a
   garbage return value.
6. Track names: the old plugin mapped `EItemType` → DLBB names (`FPP_Pistol_Show`…). `ItemType_Pistol` also
   covers SMGs and `ItemType_TwoHandedHeavyStick` covers the Gnome. DL1 actually names its tracks after the
   weapon's `AnimPrefix` (see §5).

---

## 4. Compiled animation-script bank (`_ANIMATION_SCR_`, rpack type 66/67)

Fully decoded — parse → serialize is byte-identical on all retail and DevTools banks.

**part00 (ANIMSCR, section type 0x42):**
```
nrec × 56-byte records, sorted by lower-case track name
  +0  u32 name offset (into name table)     +4  u32 per-build word (0x24C retail, 0 DevTools)
  +8  u64 0                                 +16 u32 loopCount   +20 f32 blendTime
  +24 f32 fps   +28 f32 startFrame   +32 f32 endFrame          +36..+47 0
  +48 u32 event count                       +52 u32 per-build word (0x7FF9 retail)
sum(event count) × 12-byte events, in record order:
  u16 time (= frame × 5)  u16 event type  i32 callback index (−1 = none)  i16 value  u16 0
name table: the track names, NUL-terminated, in record order
```
**part01 (ANIMSFIX, section type 0x43):**
```
u32 nrec | u32 ncb | ncb callback blocks | nrec anm2 resource names (record order, NUL-terminated)
callback block: u32 n, then n × { name\0, u32 argc, argc × typed arg }  i=int32 f=float s=string\0 v=vec3
```
No other cross-references: inserting a track = insert record + its events + its anm2 name, append callbacks.

## 5. RP6L rpack layout rules (from the official ResPackCompilerConsole output)

- Header 36 bytes, 20-byte section headers, 16-byte physical entries, 12-byte logical entries, name index, name blob
  (suffix-merged, not in logical order — reuse it and append new names).
- Logical order: BuilderInformation (2), Animation (sorted by name), AnimationScr (10 banks).
- `phys.bitfield = (logical index << 16) | section`, `log.FirstNameIndex = logical index`, phys entries contiguous.
- In-section alignment: Animation 16, ANIMSCR/ANIMSFIX 4, Builder 16. Sections are zlib.
- `build_equip_rpack.py` rebuilds the vanilla pack byte-identically when nothing changes.

---

## 6. How DL1 resolves the state name

`ResolveStateId` (gamedll RVA 0xC748D0 → inner 0xC74AD0) does **not** look the name up as given. It tries, in order,
`"Fpp_" + <AnimPrefix of the weapon in hand> + "_" + name`, `"Fpp_" + name`, `"Fpp_Unarmed_" + name`, …
(`"Fpp_"` comes from animCtrl vtable slot 0xAA0; the TPP controller's slot returns `"Tpp_"`; the prefix from
RVA 0xC66230). The landing animation passes just `"land_medium"`.

The plugin passes `"Show"`, so the played track is `fpp_<AnimPrefix>_show`:

| Track | AnimPrefix (inventory scripts) | Animation |
|-------|--------------------------------|-----------|
| `fpp_knife_show` | Knife | m_fpp_knife_equip_dlbg |
| `fpp_stick_show` | Stick (1H) | m_fpp_onehand_equip_dlbg |
| `fpp_stick2close_show` | Stick2Close | m_fpp_stick2close_equip_dlbg |
| `fpp_stick2heavy_show` | Stick2Heavy | m_fpp_stick2heavy_equip_dlbg |
| `fpp_beretta_show`, `_schofield_`, `_sixshooter_`, `_walker_` | pistols on the Beretta rig | m_fpp_pistol_equip_dlbg |
| `fpp_shotgun_show`, `fpp_shotgun_pa_show` | Shotgun, Shotgun_PA | m_fpp_shotgun_pa_equip_dlbg |
| `fpp_bow_show` | Bow | m_fpp_bow_equip_empty_dlbg |
| `fpp_gnome_show` | Gnome | m_fpp_stick2heavy_equip_dlbg |
| `fpp_singleshot_show` | Singleshot (Beretta rig) | m_fpp_pistol_equip_dlbg |
| `fpp_m16/ak47/railgun/grenadelauncher/chainsaw/submachinegun/submachinegun_alt/crossbow/shotgun_sa_show` | DL1-only long guns | m_fpp_shotgun_pa_equip_dlbg, no pump sound (Bad Blood has no rifle draw; best bone coverage 90–100 %) |
| `fpp_fist_show`, `fpp_fistinfection_show` | fists | Bad Blood FPP_Fists_Show: m_fpp_fists_examine_dlbg 50→70 @45 fps |
| `fpp_shield_show` | Shield | **native DL1 track**, kept (never triggered in vanilla) |
| `fpp_unarmed_weapon_break` | (not triggered) | m_fpp_weapon_break_dlbg |

Weapons with any other prefix (throwables, items…) resolve to the sentinel → nothing plays.
Track parameters/events are the Bad Blood SeqTracks as compiled by the official compiler; all sounds exist in DL1
(`hdr_snd_groups.scr`). All bone hashes of the equip anims exist in the DL1 FPP rig.

---

## 7. Plugin (`equip_anim\dllmain.cpp`)

| Hook / use | RVA (build 24076292) | Notes |
|------------|----------------------|-------|
| `SetWeaponSlot(weaponMgr, slot, flag) -> bool` | 0xBD6F60 | fires when the switch **starts** (old weapon still in hand) |
| `AnimUpdate(animCtrl)` | 0xC76EC0 | waits until the item in hand changes, then arms the one-shot before the original runs |
| item in hand: `weaponMgr.vtable[0x798/8](mgr, 0 main / 1 off)` | read from GetAnimPrefix+0x2F | same choice as GetAnimPrefix |
| `ResolveStateId(animCtrl, &id, &str, iModel, 0)` | 0xC748D0 | |
| `BlendCompute(weaponMgr, hand, instant)` | 0xBAEEC0 | forced `instant=1` while a Show weapon is switched in |
| `GetAnimPrefix(animCtrl, -1)` | 0xC66230 | optional, for the log |
| `Fpp_` appender (FPP vtable[0xAA0/8]) | 0xBF8170 | filters out the TPP controller |
| layout check `call [rax+0xD38]; movss [rbx+0x1830]` | 0xC2007E | landing anim |
| sentinel: `mov rax,[rip+x]; mov ecx,[rsi+0x182C]; cmp ecx,[rax]` | 0xC7A4EE | → pointer slot 0x137B210 |
| FPP hand tracks (vis vtable 0xAE8 / 0xAF0) | 0xBFF760 / 0xBFF450 | optional; Show forced on both after the original |
| target node check `lea rcx,[r14+0x1670]; call` in BlendCompute | 0xBAF3B7 | busy / weapon put away |
| weapon OnInput fire / melee / bow (vtable 0xB0) | 0xD9F890 / 0xDB3720 / 0xD9BB60 | optional, attack block |

**Playback priority (2026-10-30).** Versions 1–3 used the landing one-shot (`animCtrl+0x182C/+0x1830`,
consumer 0xC7A480): it sits after ~40 handlers, so jump/fall/land/sprint pre-empted the Show and the landing
anim overwrote it. Bad Blood plays its Show through the **first** handler of AnimUpdate (BB `FUN_180D84760`,
BB AnimUpdate 0xD89DE0): an "action state" object, played and returned before everything else. DL1 has the same
branch at the start of AnimUpdate: `*(weaponMgr+0xBF8)`, `+0x10` active, `+0x38` phase,
`+0x44 + phase*12` stateId, loop = phase 1/3, blend −1 (record blend). The plugin sets active=1, phase=0,
id0=Show around the original AnimUpdate call for the Show's duration (`vtable[0xD38/8]`) and restores the fields
right after. If the game's own action is still active when the Show is armed, the Show waits up to 300 ms
for it to end; if a game action starts mid-Show, the Show stops. A landing one-shot started during the Show is
cleared when the Show ends.

**How Bad Blood really plays Show/Hide (v5, 2026-10-30).** Traced from the `_Show`/`_Hide` strings:
- `InventoryItemController` (BB vtable 0x1817AE028) builds and caches the names: slot 0x48 = name builder
  (BB 0xC07850, "Fpp_<prefix>_Show/_Hide"), slot 0x30 caches name + duration (0xC07CC0), slot 0x18/0x20 =
  Show/Hide duration, 0x38/0x40 = Show/Hide name. All return empty/default unless `weaponMgr+0x1D7C` is set.
- `weaponMgr+0x1D7C` is the **3rd argument of BB `SetWeaponSlot`** (0xD03220). Only player-driven switches pass 1:
  the input handler 0xCE63E0 (next/prev, slots 0–3; *not* the "last weapon" key), next-weapon 0xCF73F0,
  select-slot 0x59ABB0, 0xCC8430. Pickups, restores and automatic switches pass 0. In DL1 the 3rd argument only
  resets `*(mgr+0xC10)+0x854`; the plugin logs the caller RVA of every switch (`from RVA 0x…`) to map them later.
  DL1 counterparts: input handler 0xB98450, next weapon 0xBD3070, local-player select 0x58D5F0 / 0x58D5AB.
- BB `BlendCompute` (0xD0F790, DL1 0xBAEEC0) uses the Show/Hide durations as raise/lower times and sets
  `+0x1CB9` (showing) / `+0x1CB8` (hiding) while the weight moves. Its "busy" branch is identical in DL1
  (`vtable[0x878]` = dead, 0xECCE30, movement object `+0xBA0` flag +0x48 with game param 0x30D, action `+0xBF8`):
  target item = none (weapon put away), durations 0.1 s, no Show.
- BB plays the anim on the **hand tracks**, not the main track: FPP vis vtable 0xAF0/0xAF8 (BB 0xD3F4A0 /
  0xD41C70) copy the main track, then call 0xD7C8C0, which sets stateId = current Show/Hide, blend −1, speed
  (+0x1C) 1.0, loop 0. DL1's versions are vtable **0xAE8 / 0xAF0** = 0xBFF760 (holder `*(vis+0x1A8)+0x18`,
  hand 1) / 0xBFF450 (holder +0x10, hand 0), called right after AnimUpdate by 0xC767B0; in their place DL1 calls
  0xC79270 (carried objects / vehicles) and per-hand one-shots (`vis+0x2F20..+0x2F34`).

v5 applies the BB hand-track fields after DL1's hand updates while the Show plays (`HandTracks=1`, logs
`... hand track wanted 0x.. forced to Show` when DL1 wanted something else), and detects the busy branch from
BlendCompute's result: target node `+0x1670 + hand*0x20` empty while the switched-to weapon is requested →
`weapon put away` → no Show / Show stopped. Each Show ends with `Show done: N frames in M ms`.

**v6 (2026-10-30), after test 5.** v5's log showed rifle → pistol Shows played to the end (`Show done: 196
frames`) with no hand-track correction, yet nothing visible; two weapons with the same AnimPrefix (`stick` →
`stick`) did not restart; two-handed Shows moved the camera. Findings:
- FPP anim tree (`PlayerFppVis` vtable 0x8C8 = 0xC156A0, array `*(vis+0x1A8)`, count `+0x1B0`):
  0 `MoveAnimNode`, 1 `MultislotAnimNode` main track (= vtable 0x8F0), 2 / 3 `MultislotAnimNode` hands
  (children of 1, bone masks `*(vis+0x70)+0x314` / `+0x194`), 4 / 5 `ItemAnimNode` (weapon model, child of 2 / 3,
  plays its hand's state when its own track is invalid), 6 `MimicAnimNode`.
- `MultislotAnimNode::NeedsUpdate` (0x10BCB60) only re-applies a track whose stateId differs from the playing one
  (`node+0x24`) unless the restart byte `node+0x1C` is set (cleared by the apply 0x10C32D0; the game sets it
  itself in 0xC79270). Same Show id → no restart.
- The BB pistol equip anim does not key two rig bones that the knife equip keys (hashes 6e5fc0c9, 647498d1) nor
  the weapon bones of the DL1 stands. Played on the main track these keep the previous weapon's pose; the BB
  two-handed equips key the camera. Bad Blood never plays the Show on the main track.
- BB weapon controllers (`WeaponFire/Melee/BowController`, OnInput = vtable 0xB0) return early while
  showing/hiding. DL1 OnInput: fire 0xD9F890, melee 0xDB3720, bow 0xD9BB60; owner = vtable[0x20] (= `+8`).

v6: Show only on the hand tracks by default (`MainTrack=0`; `MainTrack=1` = v4/v5), restart byte set on the
hand nodes on the first frame, weapon OnInput presses dropped during the Show (`BlockAttacks=1`, releases and
movement actions pass).

### Bad Blood special moves (v7, 2026-10-30)
Compare games from the Steam `DW\Data0.pak`: `dl1\datapacks\...\animscripts` still carries old-attempt edits.
6541 `.anm2` are shared and only 7 (TPP) differ, so BB's additions are new files plus the code that triggers them.

- **Copied tracks** (`BB_TRACKS` in `build_equip_rpack.py`, from BB's `6766_anims_player` bank, events and sounds
  included; DL1's melee code already asks for these names): knife hold attack (`m_fpp_knife_charge_attack_3hit`),
  heavy "dynamic" swings, weapon throw, mine throw. BB's normal attack wind-ups were left out on purpose (slower).
- **Events**: ids below ~700 match; above they moved. 720/721 mean PAUSE/UNPAUSE_GIVE_DAMAGE in BB but
  PVP_GRAB_* in DL1, 747/748 (motion trail) do not exist in DL1: all four are dropped. DL1 reads one damage window
  per track (FUN_180DC9393), so the knife's 3 stabs share one window: the target is hit once, not three times.
- **Skills** (player properties, `*(*(*holder+0xD0) + id*8)`, value at +8; `holder` is `player+0xE58` only when
  `player+0x50` is 0 or vtable+0x9C8 returns true, otherwise the global at RVA 0x1C122C8. PlayerDI's vtable+0x9C8 is
  `xor al,al; ret` and its +0xE58 stays null, so the player uses the global: **v7 read +0xE58 and never forced anything**,
  which the v7 log shows by having no `Skills before unlock` line. Fixed in v8, and the harness now runs the forcing on a
  fake PlayerDI. The id -> name switch FUN_180C59290
  starts with `dec edx`, so name-table index + 1 = id): ChargeAttack 0x3A5 (light melee), WhirlwindCharge 0x3A4
  (heavy, ItemType 0xF), StompLevel 0xC1 (-1 = off). Gate of the hold attack: FUN_180DBB220; stomp: StompFinisherController
  OnInput FUN_180D8C2A0 on `_ACTION_KICK` (0xA7), `_ACTION_CHARGE_ATTACK` (0x45) is on the left mouse button in both
  games. The plugin forces these values each frame (`UnlockCharge`, `UnlockWhirlwind`, `UnlockStomp`).
- **Head stomp**: `fpp_unarmed_stompfinisher_level1` is byte-identical in both games and is the only unarmed move
  besides the grab finisher with GRAB_DESTROY_HEAD. BB's skill sets `StompLevel` to -1 + 0, so whether BB reaches it
  this way is unconfirmed. The user said it is not the move they meant (that one is the jump attack below).
- **Jump attack** (v8, the move the user asked for: attack while falling on an enemy): property `JumpAttackHit`
  (DL1 id 0x3C0, BB 0x3C3), skill `DeathFromAbove`. Same code in both games: gate FUN_180DC4900 (BB FUN_180C81F00), which
  needs an attack press or hold, a non-heavy weapon and no off-hand item; target check FUN_180DC6B30 (fall start at least 1.8 m
  above the target's feet). The damage type picks the animation (FUN_180DCCAA0): Cut (1) = grab type 7 `JumpAttack`
  (the stab, `m_fpp_assassin_jumpattack`), Blunt (18) = type 8 `JumpAttackBlunt` (`m_fpp_stick_jumpattack`). All
  tracks and `.anm2` are identical in both games. The skill was **not** what blocked it: the v8 log reads
  `JumpAttackHit 1` before any forcing, the user's save already owns these skills. The move still did not start in test.
  Trigger chain, the same in both games: left click (0x9F) in the air starts a swing (`FUN_180DBA5A0` picks UD, type 2, in
  the air); type 2 is in mask 0xEC24 so `+0x770` is set; the gate then passes while action 0x45 > 0.9 (button down) or the
  charge stance `+0x328` is non-zero; each frame FUN_180DC9B70 searches targets within `JumpAttackRange`; the move starts in
  the player's collision callback (0x180B9A025 -> FUN_180DCBC50), so it needs **physical contact with the enemy**, then
  `target->vt[0x2E8](target, 1, player, 7|8)`. `_ACTION_GROUND_POUND` (0x23, key C held 0.3 s in DL1, unbound in BB) reaches
  this code only when vtable+0x878 is true, i.e. playing as the zombie; a human uses it for the ground pound.
  v9: `JumpAttackClick` counts a press in the air as a charge stance until landing (hook on the gate, `+0x328` set
  only for that call), `JumpAttackLog` writes each step to the log (`attack pressed in the air`, `gate open/closed`, `target`,
  `Contact in the air`, `landed`). v9 test (2026-10-31): all 4 contacts logged `gate 1, target 1`, the cooldown was set and the
  grab controller (`player+0xBB8`, `busy`) became active, with grab type 7 (`JumpAttack`, cutting) though the user believes
  the weapon is blunt; yet the user sees only the normal animation and says Bad Blood's version also slows the fall and
  ends with a kick. v10 logs for 3 s after a contact, one line per change: main track, the id it really plays (holder +0x24),
  both hand tracks, grab active/phase/type/set. Track id = index in the sorted `anims_player` bank (`fpp_stick_show` = 0x826,
  `fpp_unarmed_begin_jumpattack` = 0x8CE, `finishwins_jumpattack` = 0x939, blunt 0x8CF / 0x93A), -1 = name did not
  resolve. Grab branch of AnimUpdate: FUN_180C78980, name built in FUN_180C722B0 (`Begin_` + type name, table at 0xCD0BD8,
  index = type + 1). Melee controller = `player+0xC10`; `player+0xBF8` read at the top of AnimUpdate is another controller
  (the collision callback's register holds player+0x18).
  v10 test (2026-11-02, 2 contacts, one-handed `stick` weapon classed Cut): the main track really plays
  `fpp_unarmed_begin_jumpattack` (contact, in the air) then `fpp_unarmed_finishwins_jumpattack` from landing for 1.3 s, hand
  tracks -1 (no override, the arms follow the main track), then `fpp_stick_stand`. So DL1 plays its stab, which is also
  Bad Blood's move against zombies (same `.anm2` byte for byte, same zombie reactions `mz_deadly_finish*` in `anims_man_all`).
  The move the user remembers (fall slowed, landing on the target, kick at the end) is BB's **PvP** "death from above",
  no-kill version: grab `HumanJumpAttack`, BB-only tracks `fpp_/tpp_pvphumanjumpattack_attacker_*` (pounce 0-30, tackle 30-40,
  land 40-74, release 74-115 of `m_fpp_attacker_pvpgrab_dropattack_nokill`, absent from DL1) and the matching `victim_*`
  tracks plus `*_victimdied` variants. Its victim animations exist only in the player bank, none in any BB AI bank, so in BB
  it targets another player, not a zombie. BB trigger: FUN_180C45010 (needs `JumpAttackHit`, the gate FUN_180C81F00, vertical
  speed below -`f_btz_jump_attack_min_fall_speed`, a cone query FUN_180C42AA0 with `f_btz_jump_attack_range`,
  `_range_velocity_factor`, `_angle_max`, `_source_position_forward_offset`, filter FUN_180C43C10 -> FUN_180C43B30), then
  cooldown 6 and FUN_180C38F80(grab manager, target, 2, 0, dir). DL1 has the same variables (BTZ human vs Night Hunter,
  `m_fpp_human_pvp_deathfromabove`, kill version only). The "slowed fall" is the callback `activateslowmo(0.6, 0.2, 0.4)`
  in the pounce track (same in both games).
  v11 (2026-11-02, data, not yet tested; the user wants the move on zombies): the builder (`DROP_TRACKS`, `--no-drop-attack`
  to keep the stab) redefines the jump attack tracks in `anims_player`, `anims_player_man_all`, `anims_man_all` and
  `anims_hellraid_all`. Player `fpp_/tpp_unarmed_begin_jumpattack[blunt]` = attacker 30-40 (tackle),
  `finishwins_jumpattack[blunt]` = attacker 40-115 (land + release with the kick at track frame 30); AI
  `mz_/mzi_/w_/m_ncp_/m_npc_[begin_]deadly_finish[_blunt]` = victim (full-body man-skeleton animation, which BB also plays on
  third-person players) on the same ranges. The AI picks these through its anim graph parameter `grab_type` =
  `deadly_finish` / `deadly_finish_blunt` (enum at FUN_180662590), used only by this grab. Events: DL1's grab events kept
  (`GIVE_GRAB_DAMAGE` 611 at the landing, frame 8; `GRAB_RELEASE` 613 and `CAN_WALK` 610 at the end; AI IK events at 0,
  `EVENT_FORCE_FALL` 94 / `EVENT_LAY_DOWN` 177 / 1102 at the end), the stab's sounds and blood dropped, BB's speech, slow
  motion and hit sound added on the player's begin, rumble on landing and kick, wound effect on the AI at frame 9 (fpp tracks
  only). The spitter (own skeleton) keeps the stab. Unknowns for the test: whether the grab aligns the zombie with the BB
  pair (authored for two players), whether the zombie survives the damage, and how the end of the victim animation looks.
  The trace now covers 5 s.
  v11 test (2026-11-02): the user confirms the move plays right (landing, kick), but the camera stays level instead of
  looking down. The DL1 player grab never calls the look setter, so the BB attacker animation (whose camera seems
  level, BB's PvP grab system handling the view) shows the horizon.
  v12 (2026-11-02, **not yet tested**): `JumpAttackLookDown` (degrees, default 40, 0 = off). From the contact
  (`hkJumpHit`, gate and target passed) until the grab of type 7/8 ends (or 6 s), after each AnimUpdate the player's look
  pitch is held at min(pitch at the contact, -JumpAttackLookDown). Look angles, degrees, negative = down: `+0x116C`
  aim yaw, `+0x1170` aim pitch (melee code compares it with -35 / -25), `+0x1174` / `+0x1178` camera yaw / pitch
  (integrated from the mouse by FUN_180BA1350), `+0x11EC` yaw used by the contact code. Offsets read from the look setter
  FUN_180BA1C00(player, dir, notify) (signature in the plugin, movss at +0x4D / +0x77), called only by vehicle exit and
  spawn code. The log writes `camera held at`, each value the game writes in between (`game set the camera to`) and
  `camera released`. If the camera does not move, the FPP camera ignores the look pitch during grabs: next lead is the
  camera code (CameraFPPDI vtable 0x18147FF48).
  v12 test (2026-11-03): the hold works (the FPP camera uses `+0x1178` during the grab). The log shows the grab writing
  0 to `+0x1178` once, on its first frame, and nothing after; the user looked at -70 at the contact, so the view stayed on
  the feet. The user then explained that in Bad Blood the camera follows the victim, also when it is kicked away.
  v13 (2026-11-03, **not yet tested**): `JumpAttackCamera` (default 1) replaces `JumpAttackLookDown`. Each frame the
  pitch is aimed at the enemy, smoothed (1 - e^(-10 dt), ~0.3 s), clamped to -85..30: eye = FPP model bone `head`
  (model from animCtrl+0x40 vt[1]), target = enemy bone `spine2` (`spine1`, `spine`, `pelvis`), fallbacks player
  position + 1.6 m / enemy position (`vt[0x270]`, cached at `+0x808`) + 0.6 m, bones accepted within 3 m / 4 m of the
  object. Engine exports from `engine_x64_rwdi.dll`: `IModelObject::GetElementOrBoneElementID(const char*)` and
  `GetElementWorldPos(int)` (hidden return pointer). An AI or PlayerDI object is its own IModelObject (AI code at
  FUN_1805EC4D0 calls `vt[0x270]` and GetElementWorldPos on the same pointer; AI, HumanAI and PlayerDI share
  `vt[0x270]` = FUN_180191C60). Yaw is left to the grab. The log writes eye, enemy point and pitch every 250 ms.
  v13 test (2026-11-03): the camera follows the zombie (-85 under the player, -15 once kicked ~4.6 m away). The user
  reports the zombie getting up on its knees for ~0.5 s before its ragdoll. Traced chest height: lying 99.08-99.17 from
  +750 to +2750 ms, slide +1750..+2500 ms, up to 99.61 at +3000 ms; finish phase from ~+580 ms (begin stretched by the
  slow motion), so the slide ends at anim frame ~98 and the rise starts ~106.
  v14 (2026-11-03, **not yet tested**): AI finish tracks cut to 40-98 (`DROP_FINISH_VICTIM`), fall / lay down at track
  frame 57; the player's track is unchanged. The track trace also logs the hand nodes' playing ids (`+0x24`) and runs
  during the first 10 firearm Shows (the user still sees no pistol equip anim; no pistol switch in the v9-v13 logs).
  hkAnimUpdate now runs the trace and the camera after every path (AnimUpdateShow holds the Show logic).
  v15 (2026-11-03, **not yet tested**; the user asked for a real impact and an earlier camera release):
  - `GRAB_DESTROY_HEAD` (622, value 1) on the player's finish tracks (fpp and tpp) at track frame 30, the kick. It is the
    event DL1's grab finisher `fpp_unarmed_finishwins_scalping_1` fires at its frame 20 (also in `m_fpp_legs.scr`, the
    stomp finisher, value -1). No cut-in-half event exists in the banks.
  - The camera is given back `JumpAttackCameraTime` seconds (default 1.5, 0 = at the end) after the landing, i.e. the
    grab phase change 0 -> 6 (`grab+0xCC`; +578 ms after the contact in the v13 trace). At 1.5 s the zombie is ~2.9 m away
    and still sliding (slide ends ~+2.5 s).
  v15 test (2026-11-03): camera release liked; the head does not explode; the zombie freezes 0.5-1 s at the end of its
  track before its ragdoll (no pistol switch in the log: the 4 quick slots hold melee weapons).
  - Head: GrabController (vtable 0x18172D1B0) anim event handler vt[0xC0] = FUN_180D26750: 611 -> FUN_180D26830 (damage),
    622 / 623 -> FUN_180D26BF0(grab, 0 / 1) only if byte `grab+0xDC`, 613 -> `grab+0x48` = 0. The grab start
    FUN_180D222A0 sets +0xDC = 1; the update FUN_180D24280 clears it for grab types 7-16 when FUN_180BBCB50(player) is
    true and `player->vt[0xA20](FUN_1800A8FE0(3) = 128)` < 0.7 (a skill/stat value, not a player property; DL1's scalping
    finisher is type 10, in that range). FUN_180D26BF0 also needs the item "FinisherFront_Item" and the victim refs
    `+0x90` / `+0x98`.
  - Freeze: a grabbed AI only falls once released. DL1's back finisher releases mid-track: player `GRAB_RELEASE` 613 at 100,
    `CAN_WALK` 610 at 110 of 122, AI `EVENT_GRAB_RELEASE` (29) at 110. AI event names from the .scr pairing: 7
    EVENT_FALL, 29 EVENT_GRAB_RELEASE, 94 EVENT_FORCE_FALL, 1040/1041 VIS_EVENT_IK_ENABLE/DISABLE, 1043
    VIS_EVENT_IK_DISABLING_START, 1061 VIS_EVENT_LEFT_HAND_IK_STOP, 1100 VIS_EVENT_STOPANIM, 1102
    VIS_EVENT_ORIGINAL_BODY_SCALE, 1103 VIS_EVENT_BLOCK_GENERIC_MIMIC.
  v16 (2026-11-03, **not yet tested**): player finish track `GRAB_RELEASE` at 55, `CAN_WALK` at 62 (the track still runs to
  75); AI finish track fall / lay down at 56, `EVENT_GRAB_RELEASE` at 57. Plugin hooks FUN_180D26750 (`JumpAttackHead`,
  default 1; flag offset read from its `cmp byte [rbx+0xDC],0` at +0x3F) and sets `grab+0xDC` = 1 just before a 622 of
  grab type 7 / 8; logs `head destroyed by the kick (flag was N)`.
  v16 test (2026-11-03): the head explodes, but at the kick; the user wants it at the weapon's impact. The log showed the
  flag already at 1 when the 622 arrived, so the hook did not change anything this time.
  v17 (2026-11-03, **not yet tested**): `GRAB_DESTROY_HEAD` moved to finish frame 8, with `GIVE_GRAB_DAMAGE` (Bad Blood's
  victim takes its damage there, the landing impact); the log writes the time after the contact.
  Differences found: BB also requires a 2.7 m fall and 0.3 s in the air (`JumpAttackMinFallHeight`,
  `JumpAttackMinTimeInAir`, FUN_180C8D150); DL1 has 0.2 s hard-coded (FUN_180DC64F0) and no such properties. DL1's target check
  also refuses targets with `+0xC24` in 3..12 and has a level-dependent branch (FUN_180460C50) that requires the fall start
  `+0x1228` to be above 0 in world height on other levels; BB requires above 0 everywhere. `f_btz_jump_attack_*` (DL1
  FUN_180D611E0) is the PvP version described above.
- **Knife wind-up**: neither game has `fpp_knife_melee_attack_ja_*`, only stick and stick2close do. Knives pass the
  gate, so `CLONED_TRACKS` adds them from the stick tracks. Whether the game requests these names for a knife is
  not confirmed in-game.
- **Ground pound** (heavy weapons): property `GroundPound` (id 0x220, float > 0), check at 0xDB392C; BB's
  `gp_empty` (50-116 at 40 fps) and `gp_empty_rest` are copied. `GroundPoundMinHeight` (id 0x225) is forced to BB's 3.0
  (DL1 1.1) by the ini value of the same name. `GroundPoundDamageMul` stays DL1's (25, BB 1.5).
- **Not ported, need BB-only code**: parry/block reactions (`_ACTION_PARRY`, `TakeDamageParry*`), charged bow shot
  (`ShootCharged`), weapon break (`Unarmed_weapon_break`), crawl/downed state, weapon examine.
  `FPP_Unarmed_Chraged_Attack_Kick` is not requested by BB's own DLL (string absent).

`equip_anim.ini`: `Enabled`, `Log`, `AnimName` (default Show), `InstantSwitch` (default 1), `InstantPrefixes`,
`HandTracks` (default 1), `MainTrack` (default 0), `BlockAttacks` (default 1), `UnlockCharge`, `UnlockWhirlwind`,
`UnlockStomp`, `UnlockJumpAttack`, `UnlockGroundPound` (default 1), `GroundPoundMinHeight` (default 3.0, 0 = keep the
game's), `JumpAttackClick`, `JumpAttackLog` (default 1), `JumpAttackCamera` (default 1), `JumpAttackCameraTime` (default 1.5), `JumpAttackHead` (default 1), `MaxDuration`.

### The lower/raise "lerp" (solved 2026-10-29)
Weapon switching is a per-hand state machine, not an animation:
- `SetWeaponSlot` only fills the **requested** nodes `weaponMgr+0x16B0` (main) / `+0x16D0` (off hand).
- Every frame `FUN_180BAEE40(mgr, instant)` → `BlendCompute(mgr, 1, instant)` then `(mgr, 0, instant)`.
- `BlendCompute` lowers the hand weight `+0x17AC + hand*4` (0x17AC = main, 0x17B0 = off hand — *not*
  new/old) with a default **0.2 s** (`0x1926CA4`), then `FUN_180BAEBE0(mgr, hand, item)` moves the item into the
  **active** node `+0x1630 + hand*0x20` (what `vtable[0x798](mgr, hand)` and the animation system read), then the
  weight is raised again.
- `instant != 0` zeroes both durations: swap + raise in one frame — Bad Blood's behaviour.
The plugin passes `instant=1` from SetWeaponSlot until the equip anim is armed (+100 ms), only when the requested
weapon's AnimPrefix is in `InstantPrefixes` (otherwise weapons without a Show anim would pop in).
The March "snap +0x17AC=1 / +0x17B0=0" was wrong (it hid the off hand) and is gone.

### In-game check (`equip_anim.log`, next to the .asi)
```
  SetWeaponSlot   RVA 0xBD6F60  ... FppPrefix RVA 0xBF8170 / sentinel 0x....  / Hooks enabled
[SetWeaponSlot] mgr=... slot 0 -> 1
  prefix 'Knife': Fpp_Knife_Show id=0x... 0.667s        <- played
  prefix 'Submachinegun': no Fpp_Submachinegun_Show track, skipped
```
First in-game test (2026-10-29): arming right after SetWeaponSlot played the *previous* weapon's Show
(knife → katana played the knife anim, then the katana appeared with no anim). Fixed by waiting for the item in
hand to change (`active ... after N ms` line). Second test: correct anim but ~250 ms of vanilla lower first →
InstantSwitch (below). With it the log should show `requested 'knife' (instant)` and `active ... after 0-30 ms`.

---

## 8. Older findings (pre-update build, kept for reference)

- Weapon manager: `+0x2998` weapon array (stride 0x18, count at +0x29A0), `+0x2A50` slot, `+0x2A54` previous,
  `+0xCFC` current slot (−1 = none), `+0x16B0/+0x16D0` hide/show list nodes, `+0x17AC/+0x17B0` blend weights.
- `weapon+0x58` component: vtable[0x1C0/8] = EItemType (0x0C Knife, 0x0D Stick, 0x0F 2H heavy, 0x10 2H close,
  0x12 Pistol, 0x13 Revolver, 0x18 Shotgun, 0x23 Bow).
- `*(weaponMgr+0xBF8)` = native DL1 "show state" struct checked first by AnimUpdate (+0x10 active, +0x38 phase,
  +0x44 + phase×12 stateId); never set in vanilla.
- Lerp experiments (writing +0x118/+0x124 of the lerp global, removing the +0x16B0 node) failed — see
  `equip_anim\lerp_suppress.cpp`.
- Bad Blood triggers its equip anims from FUN_C06C50 (prefix + "_Show"/"_Hide"), absent from DL1.
