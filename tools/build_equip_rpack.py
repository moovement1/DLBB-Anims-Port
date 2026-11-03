"""Build Dying Light 1's common_anims_PC.rpack with the weapon equip ("Show") animations.

The game DLL resolves one-shot animations by composing  "Fpp_" + <weapon AnimPrefix> + "_" + <name>
(gamedll FUN_180C74AD0).  equip_anim.asi asks for <name> = "Show", so every weapon whose AnimPrefix has a
"fpp_<prefix>_show" track gets an equip animation and every other weapon is left untouched.

What this script does (stdlib only, no game tools needed):
  1. keeps a pristine backup of the game's rpack (first run only) and always builds from that backup;
  2. grafts the Show tracks into both player animation banks (anims_player, anims_player_man_all);
     the attack while falling on an enemy becomes Bad Blood's drop attack in the player and AI banks
     (--no-drop-attack keeps DL1's stab);
  3. adds the Bad Blood equip .anm2 resources to the Animation section;
  4. rebuilds the RP6L pack using the layout rules of the official ResPackCompilerConsole;
  5. re-reads the result and checks that every original resource is byte-identical before installing.

Usage:
  python build_equip_rpack.py --game "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dying Light" \
                              --anims "C:\\...\\dlbb\\unpack_dlbb\\Animation" [--install]

Track parameters/events below are the Bad Blood SeqTrack definitions as compiled by the official
Dying Light ResPackCompilerConsole (build of 2026-03-04), re-keyed to DL1 AnimPrefix names.
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys
import zlib

# ---------------------------------------------------------------------------------------------
# Show track definitions
#   events: (time, type, value, callback block hex | None); time unit = frame * 5
# ---------------------------------------------------------------------------------------------
_CB_KNIFE = '01000000706c6179736f756e64326465787465726e616c0005000000690400000073776561706f6e5f6b6e6966655f73686f772e77617600660000803f69010000006901000000'
_CB_1H = '01000000706c6179736f756e64326465787465726e616c0005000000690400000073776561706f6e5f31685f73686f772e77617600660000803f69010000006901000000'
_CB_2H = '01000000706c6179736f756e64326465787465726e616c0005000000690400000073776561706f6e5f32685f73686f772e77617600660000803f69010000006901000000'
_CB_2H_HEAVY = '01000000706c6179736f756e643233640008000000690400000073776561706f6e5f32685f68656176795f73686f772e77617600660000803f66000040407368656164006901000000760000000000000000000000006901000000'
_CB_HAMMER_GRIP = '01000000706c6179736f756e643233640008000000690400000073656d7074795f68616d6d65725f68656176795f677269702e77617600660000803f66000040407368656164006901000000760000000000000000000000006901000000'
_CB_FIST_SPEECH = '01000000706c6179706c61796572736f756e643600060000006903000000735370656563685f466973745f53686f775f58582e77617600660000803f6900000000690100000066cdcccc3e'
_CB_BERETTA = '01000000706c6179736f756e64326465787465726e616c0005000000690400000073626572657474615f72656c6f61645f706172745f30312e77617600660000803f69010000006901000000'
_CB_SHOTGUN = '01000000706c6179736f756e64326465787465726e616c000500000069040000007353686f7467756e425f72656c6f61645f58582e77617600663333333f69010000006901000000'
_CB_GLASS = '01000000706c6179736f756e64323364000800000069070000007364657374726f7961626c655f77696e646f775f676c6173735f30312e77617600660000803f66000040407368656164006901000000760000000000000000000000006901000000'

ANIMS = {
    'knife':        dict(anm2='m_fpp_knife_equip_dlbg',       loop=1, blend=0.0, fps=30.0, start=0.0, end=20.0,
                         events=[(5, 0, 2, _CB_KNIFE)]),
    'onehand':      dict(anm2='m_fpp_onehand_equip_dlbg',     loop=1, blend=0.0, fps=30.0, start=0.0, end=20.0,
                         events=[(0, 0, 2, _CB_1H), (5, 0, 2, _CB_FIST_SPEECH)]),
    'stick2close':  dict(anm2='m_fpp_stick2close_equip_dlbg', loop=1, blend=0.0, fps=30.0, start=0.0, end=20.0,
                         events=[(5, 0, 2, _CB_2H), (45, 0, 2, _CB_FIST_SPEECH)]),
    'stick2heavy':  dict(anm2='m_fpp_stick2heavy_equip_dlbg', loop=1, blend=0.0, fps=30.0, start=0.0, end=20.0,
                         events=[(0, 0, 2, _CB_2H_HEAVY), (55, 0, 2, _CB_HAMMER_GRIP), (55, 0, 2, _CB_FIST_SPEECH)]),
    'pistol':       dict(anm2='m_fpp_pistol_equip_dlbg',      loop=1, blend=0.0, fps=30.0, start=0.0, end=20.0,
                         events=[(5, 0, -1, _CB_BERETTA)]),
    'shotgun':      dict(anm2='m_fpp_shotgun_pa_equip_dlbg',  loop=1, blend=0.1, fps=23.0, start=0.0, end=20.0,
                         events=[(25, 0, -1, _CB_SHOTGUN)]),
    # DL1-only long guns (rifles, SMGs, chainsaw, crossbow): same two-handed draw, without the pump sound
    'longgun':      dict(anm2='m_fpp_shotgun_pa_equip_dlbg',  loop=1, blend=0.1, fps=23.0, start=0.0, end=20.0,
                         events=[]),
    # Bad Blood FPP_Fists_Show (m_fpp_fists.scr); its m_fpp_fists_show.wav does not exist in DL1
    'fists':        dict(anm2='m_fpp_fists_examine_dlbg',     loop=1, blend=0.0, fps=45.0, start=50.0, end=70.0,
                         events=[(5, 0, 2, _CB_FIST_SPEECH)]),
    # Bad Blood FPP_Shield_Show (m_fpp_shield.scr); the animation already ships with DL1
    'shield':       dict(anm2='m_fpp_shield_actions',         loop=1, blend=0.3, fps=20.0, start=0.0, end=9.0,
                         events=[]),
    'bow':          dict(anm2='m_fpp_bow_equip_empty_dlbg',   loop=1, blend=0.1, fps=30.0, start=0.0, end=22.0,
                         events=[]),
    'weapon_break': dict(anm2='m_fpp_weapon_break_dlbg',      loop=1, blend=0.1, fps=30.0, start=0.0, end=30.0,
                         events=[(5, 0, 1, _CB_GLASS), (125, 612, 1, None)]),
}

# track name -> animation; "fpp_<AnimPrefix>_show" where AnimPrefix comes from data/scripts/inventory
TRACKS = {
    'fpp_knife_show':        'knife',        # ItemType_Knife
    'fpp_stick_show':        'onehand',      # ItemType_Stick / Hook
    'fpp_stick2close_show':  'stick2close',  # ItemType_TwoHandedCloseStick
    'fpp_stick2heavy_show':  'stick2heavy',  # ItemType_TwoHandedHeavyStick
    'fpp_gnome_show':        'stick2heavy',  # TwoHandedHeavyStick with its own prefix
    'fpp_beretta_show':      'pistol',       # pistols sharing the Beretta rig
    'fpp_schofield_show':    'pistol',
    'fpp_sixshooter_show':   'pistol',
    'fpp_walker_show':       'pistol',
    'fpp_singleshot_show':   'pistol',
    'fpp_shotgun_show':      'shotgun',
    'fpp_shotgun_pa_show':   'shotgun',
    'fpp_shotgun_sa_show':   'longgun',
    'fpp_m16_show':          'longgun',      # ItemType_Rifle
    'fpp_ak47_show':         'longgun',
    'fpp_railgun_show':      'longgun',
    'fpp_grenadelauncher_show': 'longgun',
    'fpp_chainsaw_show':     'longgun',
    'fpp_submachinegun_show':     'longgun',  # ItemType_Pistol, but held with both hands
    'fpp_submachinegun_alt_show': 'longgun',
    'fpp_crossbow_show':     'longgun',
    'fpp_bow_show':          'bow',
    'fpp_fist_show':         'fists',        # ItemType_Fists / Knuckles
    'fpp_fistinfection_show': 'fists',
    'fpp_shield_show':       'shield',
    'fpp_unarmed_weapon_break': 'weapon_break',  # not triggered by the plugin, shipped for completeness
}
# ---------------------------------------------------------------------------------------------
# Bad Blood attack animations: copied with their events and sounds from Bad Blood's compiled player
# bank. DL1's melee code already asks for these names, so no trigger is needed. Normal attack wind-ups
# (fpp_stick_melee_attack_rl_begin...) are left alone: Bad Blood's are slower and change the combat feel.
# ---------------------------------------------------------------------------------------------
BB_BANK = '6766_anims_player'
BB_TRACKS = (
    # hold attack with a knife: the 3 stabs, from the same wind-up on both sides
    'fpp_knife_melee_attack_ch_rl_empty',
    'fpp_knife_melee_attack_ch_rl_empty_rest',
    'fpp_knife_melee_attack_ch_lr_begin',
    'fpp_knife_melee_attack_ch_lr_loop',
    'fpp_knife_melee_attack_ch_lr_empty',
    'fpp_knife_melee_attack_ch_lr_empty_rest',
    # two-handed heavy weapons: the "dynamic" swings, normal and charged
    'fpp_stick2heavy_idle_rest',
    'fpp_stick2heavy_melee_attack_rl_begin',
    'fpp_stick2heavy_melee_attack_rl_empty',
    'fpp_stick2heavy_melee_attack_rl_empty_rest',
    'fpp_stick2heavy_melee_attack_ud_empty',
    'fpp_stick2heavy_melee_attack_ud_empty_rest',
    'fpp_stick2heavy_melee_attack_ch_rl_begin',
    'fpp_stick2heavy_melee_attack_ch_rl_empty',
    'fpp_stick2heavy_melee_attack_ch_rl_empty_rest',
    'fpp_stick2heavy_melee_attack_ch_lr_begin',
    'fpp_stick2heavy_melee_attack_ch_lr_loop',
    'fpp_stick2heavy_melee_attack_ch_lr_empty',
    'fpp_stick2heavy_melee_attack_ch_lr_empty_rest',
    # weapon throw
    'fpp_knife_melee_throw',
    'fpp_stick_melee_throw',
    'fpp_stick2close_melee_throw',
    # mine throw
    'fpp_mine_throwbegin',
    'fpp_mine_throwready',
    'fpp_mine_throwattack',
    'fpp_mine_fastthrow',
    # ground pound with a two-handed heavy weapon: longer, faster swing down
    'fpp_stick2heavy_melee_attack_gp_empty',
    'fpp_stick2heavy_melee_attack_gp_empty_rest',
)
# Knives have no wind-up for the attack while falling, in either game; only stick and stick2close do.
# Their damage type allows the move, so they get the one-handed wind-up, which fires EVENT_JUMP_ATTACK.
CLONED_TRACKS = {
    'fpp_knife_melee_attack_ja_begin': 'fpp_stick_melee_attack_ja_begin',
    'fpp_knife_melee_attack_ja_empty': 'fpp_stick_melee_attack_ja_empty',
}
# Bad Blood event ids DL1 does not have. DL1 reads one damage window per track (first START/END_GIVE_DAMAGE,
# FUN_180DC9393), so the pauses inside the knife's 3 stabs are dropped: one window covering the 3 stabs.
BB_EVENTS_DROPPED = {720: 'PAUSE_GIVE_DAMAGE', 721: 'UNPAUSE_GIVE_DAMAGE',
                     747: 'WEAPON_MOTION_TRAIL_START', 748: 'WEAPON_MOTION_TRAIL_END'}
# event ids checked to mean the same in both games (.scr names paired with both compiled banks)
BB_EVENTS_SAME = {0, 501, 510, 543, 545, 546, 547, 548, 612}
# ---------------------------------------------------------------------------------------------
# Attack while falling on an enemy (grab types JumpAttack / JumpAttackBlunt). Both games stab (or crush the
# head) with the same animations. The move wanted here is Bad Blood's PvP drop attack, no-kill version
# (land on the enemy, kick it away), which Bad Blood only does on another player. Its animations replace
# the stab on both sides of the grab: the player plays the attacker's, the AI the victim's (a full-body
# man-skeleton animation that Bad Blood also plays on third-person players).
# Events keep DL1's grab meaning (Bad Blood's PvP events are not used); sounds, slow motion and the wound
# effect come from Bad Blood's first-person tracks; the weapon's impact destroys the head like DL1's scalping finisher.
# ---------------------------------------------------------------------------------------------
DROP_ATTACKER = 'm_fpp_attacker_pvpgrab_dropattack_nokill'
DROP_VICTIM = 'm_fpp_victim_pvpgrab_dropattack_nokill'
DROP_BANKS = ('anims_player', 'anims_player_man_all', 'anims_man_all', 'anims_hellraid_all')
DROP_BEGIN = (30.0, 40.0)    # Bad Blood "tackle": the hit, still in the air
DROP_FINISH = (40.0, 115.0)  # "land" + "release": on the enemy, the kick (frame 30), back on the feet
# The victim gets up on its knees from frame ~106 (it survives in Bad Blood); the AI stops at 98, lying still
# after the slide (traced: chest height flat from the end of the slide, rising ~0.25 s before the old end).
DROP_FINISH_VICTIM = (40.0, 98.0)
DROP_TRACKS = {  # track -> role; the spitter has its own skeleton and keeps the stab
    'fpp_unarmed_begin_jumpattack': 'begin_player', 'fpp_unarmed_begin_jumpattackblunt': 'begin_player',
    'tpp_unarmed_begin_jumpattack': 'begin_player', 'tpp_unarmed_begin_jumpattackblunt': 'begin_player',
    'fpp_unarmed_finishwins_jumpattack': 'finish_player', 'fpp_unarmed_finishwins_jumpattackblunt': 'finish_player',
    'tpp_unarmed_finishwins_jumpattack': 'finish_player', 'tpp_unarmed_finishwins_jumpattackblunt': 'finish_player',
    'mz_begin_deadly_finish': 'begin_victim', 'mz_begin_deadly_finish_blunt': 'begin_victim',
    'mzi_begin_deadly_finish_blunt': 'begin_victim', 'w_begin_deadly_finish': 'begin_victim',
    'w_begin_deadly_finish_blunt': 'begin_victim', 'm_ncp_begin_deadly_finish': 'begin_victim',
    'm_npc_begin_deadly_finish_blunt': 'begin_victim',
    'mz_deadly_finish': 'finish_victim', 'mz_deadly_finish_blunt': 'finish_victim',
    'mzi_deadly_finish_blunt': 'finish_victim', 'w_deadly_finish': 'finish_victim',
    'w_deadly_finish_blunt': 'finish_victim', 'm_ncp_deadly_finish': 'finish_victim',
    'm_npc_deadly_finish_blunt': 'finish_victim',
}
# Bad Blood callbacks added to the first-person and AI tracks: (frame, Bad Blood track, index of its event)
DROP_BB_CALLBACKS = {
    'begin_player':  [(0, 'fpp_pvphumanjumpattack_attacker_pounce', 0),   # speech
                      (0, 'fpp_pvphumanjumpattack_attacker_pounce', 2),   # slow motion 0.6: the slowed fall
                      (0, 'fpp_pvphumanjumpattack_attacker_tackle', 1)],  # hit sound
    'finish_player': [(9, 'fpp_pvphumanjumpattack_attacker_land', 0),     # rumble on landing
                      (32, 'fpp_pvphumanjumpattack_attacker_land', 1)],   # rumble on the kick
    'begin_victim':  [],
    'finish_victim': [(9, 'fpp_pvphumanjumpattack_victim_land', 1)],      # wound effect on landing
}


def drop_range(role):
    if role.startswith('begin'):
        return DROP_BEGIN
    return DROP_FINISH_VICTIM if role == 'finish_victim' else DROP_FINISH


GIVE_GRAB_DAMAGE = 611
GRAB_RELEASE = 613
CAN_WALK = 610
DROP_DAMAGE_FRAME = 8  # landing on the enemy, where Bad Blood applies its damage
# The kick blows the enemy's head off, with the event DL1's grab finisher fpp_unarmed_finishwins_scalping_1 uses
# (GrabController FUN_180D26750; the plugin keeps its "head can be destroyed" flag +0xDC set for this move).
GRAB_DESTROY_HEAD = 622
DROP_HEAD_FRAME = 8    # the weapon's impact on landing, with the damage (the kick at 30 felt late)
# Release like DL1's back finisher (player GRAB_RELEASE, the AI's own EVENT_GRAB_RELEASE a few frames later): a
# grabbed AI only falls once released, the release at the end of the player's track froze it ~0.6 s.
DROP_RELEASE_FRAME = 55   # player: the AI track ends at 58
DROP_WALK_FRAME = 62      # player can move; its track goes on to 75
EVENT_FORCE_FALL = 94
EVENT_GRAB_RELEASE = 29   # AI side

PLAYER_BANKS = ('anims_player', 'anims_player_man_all')
RPACK_REL = os.path.join('DW', 'Data', 'common_anims_PC.rpack')
BACKUP_SUFFIX = '.equip_anim_backup'

TYPE_ANIMATION = 64
TYPE_ANIM_SCR = 66


def fail(msg):
    print('[ERROR] ' + msg)
    sys.exit(1)


# ---------------------------------------------------------------------------------------------
# Compiled animation-script bank (_ANIMATION_SCR_)
#   part00 (ANIMSCR):  nrec x 56-byte records sorted by name | sum(evcount) x 12-byte events | names
#   part01 (ANIMSFIX): u32 nrec | u32 ncb | ncb callback blocks | nrec anm2 names (record order)
# ---------------------------------------------------------------------------------------------
REC = 56
EV = 12


def _cstr(d, o):
    e = d.index(b'\0', o)
    return d[o:e], e + 1


class Bank:
    def __init__(self, p0, p1):
        nrec, ncb = struct.unpack_from('<II', p1, 0)
        o = 8
        self.cbs = []
        for _ in range(ncb):
            st = o
            n = struct.unpack_from('<I', p1, o)[0]
            o += 4
            for _ in range(n):
                _, o = _cstr(p1, o)
                argc = struct.unpack_from('<I', p1, o)[0]
                o += 4
                for _ in range(argc):
                    t = p1[o]
                    o += 1
                    if t in b'if':
                        o += 4
                    elif t == ord('s'):
                        _, o = _cstr(p1, o)
                    elif t == ord('v'):
                        o += 12
                    else:
                        raise ValueError('unknown callback argument type 0x%02X at 0x%X' % (t, o - 1))
            self.cbs.append(bytes(p1[st:o]))
        anms = p1[o:].split(b'\0')
        if len(anms) != nrec + 1 or anms[-1] != b'':
            raise ValueError('anm2 name list does not match record count')
        recs = [bytearray(p0[i * REC:(i + 1) * REC]) for i in range(nrec)]
        nev = sum(struct.unpack_from('<I', r, 48)[0] for r in recs)
        strbase = nrec * REC + nev * EV
        self.tracks = []
        eo = nrec * REC
        for i, r in enumerate(recs):
            name, _ = _cstr(p0, strbase + struct.unpack_from('<I', r, 0)[0])
            cnt = struct.unpack_from('<I', r, 48)[0]
            evs = [bytes(p0[eo + k * EV:eo + (k + 1) * EV]) for k in range(cnt)]
            eo += cnt * EV
            self.tracks.append({'name': bytes(name), 'rec': r, 'events': evs, 'anm2': bytes(anms[i])})

    def names(self):
        return {t['name'] for t in self.tracks}

    def track(self, name):
        for t in self.tracks:
            if t['name'] == name:
                return t
        return None

    def add_track(self, name, a):
        tpl = self.track(b'fpp_knife_stand') or self.tracks[0]
        rec = bytearray(tpl['rec'])  # keeps the per-build words at +4 / +52
        struct.pack_into('<Iffff', rec, 16, a['loop'], a['blend'], a['fps'], a['start'], a['end'])
        rec[8:16] = bytes(8)
        rec[36:48] = bytes(12)
        events = []
        for time, etype, value, cb in a['events']:
            idx = -1
            if cb is not None:
                idx = len(self.cbs)
                self.cbs.append(bytes.fromhex(cb))
            events.append(struct.pack('<HHihH', time, etype, idx, value, 0))
        self.tracks.append({'name': name, 'rec': rec, 'events': events, 'anm2': a['anm2'].encode()})

    def copy_track(self, src, name):
        """Adds or replaces a track with src's, keeping this bank's per-build words (+4 / +52)."""
        s = src.track(name)
        own = self.track(name)
        rec = bytearray((own or self.track(b'fpp_knife_stand') or self.tracks[0])['rec'])
        rec[8:48] = s['rec'][8:48]
        events = []
        for e in s['events']:
            time, etype, cb, value, pad = struct.unpack('<HHihH', e)
            if etype in BB_EVENTS_DROPPED:
                continue
            if etype not in BB_EVENTS_SAME:
                raise ValueError('%s: event type %d is not known to match DL1' % (name.decode(), etype))
            idx = -1
            if cb >= 0:
                idx = len(self.cbs)
                self.cbs.append(src.cbs[cb])
            events.append(struct.pack('<HHihH', time, etype, idx, value, pad))
        if own:
            self.tracks.remove(own)
        self.tracks.append({'name': name, 'rec': rec, 'events': events, 'anm2': s['anm2']})

    def clone_track(self, src, name):
        """Adds a track identical to one of this bank's (same events, same callback indexes)."""
        s = self.track(src)
        self.tracks.append({'name': name, 'rec': bytearray(s['rec']), 'events': list(s['events']), 'anm2': s['anm2']})

    def redefine_track(self, name, anm2, start, end, events):
        """Replaces a track's animation, frame range and events, keeping its loop, blend and fps.
        events: [(frame, type, value, pad, callback block | None)]"""
        t = self.track(name)
        struct.pack_into('<ff', t['rec'], 28, start, end)
        t['anm2'] = anm2
        t['events'] = []
        for frame, etype, value, pad, cb in sorted(events, key=lambda e: e[0]):
            idx = -1
            if cb is not None:
                idx = len(self.cbs)
                self.cbs.append(cb)
            t['events'].append(struct.pack('<HHihH', int(round(frame * 5)), etype, idx, value, pad))

    def serialize(self):
        tracks = sorted(self.tracks, key=lambda t: t['name'])
        strtab = bytearray()
        offs = {}
        for t in tracks:
            offs[t['name']] = len(strtab)
            strtab += t['name'] + b'\0'
        p0 = bytearray()
        for t in tracks:
            r = bytearray(t['rec'])
            struct.pack_into('<I', r, 0, offs[t['name']])
            struct.pack_into('<I', r, 48, len(t['events']))
            p0 += r
        for t in tracks:
            for e in t['events']:
                p0 += e
        p0 += strtab
        p1 = bytearray(struct.pack('<II', len(tracks), len(self.cbs)))
        for c in self.cbs:
            p1 += c
        for t in tracks:
            p1 += t['anm2'] + b'\0'
        return bytes(p0), bytes(p1)


def drop_attack_events(name, track, role, bb_bank):
    """DL1's grab events of a jump attack track, moved onto the drop attack, plus Bad Blood's callbacks."""
    start, end = drop_range(role)
    last = end - start - 1
    out = []
    for e in track['events']:
        tm, etype, cb, value, pad = struct.unpack('<HHihH', e)
        if cb >= 0:
            continue  # the stab's sounds and blood
        frame = tm / 5.0
        if role == 'finish_player':
            frame = {GIVE_GRAB_DAMAGE: DROP_DAMAGE_FRAME, GRAB_RELEASE: DROP_RELEASE_FRAME,
                     CAN_WALK: DROP_WALK_FRAME}.get(etype, last)
        elif role == 'finish_victim':
            frame = 0 if frame == 0 else last - 1  # fall / lay down, then the release below
        out.append((min(frame, last), etype, value, pad, None))
    if role == 'finish_player':  # like the scalping finisher, on its first- and third-person tracks
        out.append((DROP_HEAD_FRAME, GRAB_DESTROY_HEAD, 1, 0, None))
    if role == 'finish_victim':
        if not any(struct.unpack('<HHihH', e)[1] == EVENT_FORCE_FALL for e in track['events']):
            raise ValueError('%s has no EVENT_FORCE_FALL' % name)
        out.append((last, EVENT_GRAB_RELEASE, -1, 0, None))
    if not name.startswith('tpp_'):  # third-person tracks run on other players: no slow motion or rumble
        for frame, src, k in DROP_BB_CALLBACKS[role]:
            tm, etype, cb, value, pad = struct.unpack('<HHihH', bb_bank.track(src.encode())['events'][k])
            if etype != 0 or cb < 0:
                raise ValueError('%s event %d is not a callback' % (src, k))
            out.append((frame, 0, value, pad, bb_bank.cbs[cb]))
    return out


# ---------------------------------------------------------------------------------------------
# RP6L resource pack
# ---------------------------------------------------------------------------------------------
class RPack:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.raw = f.read()
        d = self.raw
        (self.magic, self.version, self.flags, nphys, nsec, nlog,
         names_size, self.logres, self.align) = struct.unpack_from('<4s8I', d, 0)
        if self.magic != b'RP6L':
            raise ValueError('not an RP6L pack')
        o = 36
        self.secs = [list(struct.unpack_from('<5I', d, o + 20 * i)) for i in range(nsec)]
        o += 20 * nsec
        self.phys = [list(struct.unpack_from('<3Ihh', d, o + 16 * i)) for i in range(nphys)]
        o += 16 * nphys
        self.logs = [list(struct.unpack_from('<3I', d, o + 12 * i)) for i in range(nlog)]
        o += 12 * nlog
        name_idx = struct.unpack_from('<%dI' % nlog, d, o)
        o += 4 * nlog
        self.names_blob = d[o:o + names_size]   # compiler layout: suffix-merged, not in logical order
        self.header_end = o + names_size
        self.names = [_cstr(self.names_blob, i)[0].decode('latin1') for i in name_idx]
        self.name_offset = dict(zip(self.names, name_idx))
        self._sec_cache = {}

    def section(self, s):
        if s not in self._sec_cache:
            bf, off, size, csize, cnt = self.secs[s]
            raw = self.raw[off:off + (csize or size)]
            data = zlib.decompress(raw) if csize else raw
            if len(data) != size:
                raise ValueError('section %d size mismatch' % s)
            self._sec_cache[s] = data
        return self._sec_cache[s]

    def resources(self):
        """[(name, type, high_byte, [part bytes...], [section index...])] in logical order."""
        out = []
        for i, (bf, _first_name, first_res) in enumerate(self.logs):
            cnt = bf & 0xFF
            parts, secs = [], []
            for p in range(first_res, first_res + cnt):
                pbf, off, size, _c, _r = self.phys[p]
                s = pbf & 0xFF
                parts.append(self.section(s)[off:off + size])
                secs.append(s)
            out.append((self.names[i], (bf >> 16) & 0xFF, bf >> 24, parts, secs))
        return out


SECTION_ALIGN = {TYPE_ANIMATION: 16, TYPE_ANIM_SCR: 4, TYPE_ANIM_SCR + 1: 4}


def write_rpack(orig, resources, out_path):
    """Rebuild a pack with orig's section table. Sections whose content is unchanged keep their
    original compressed bytes, so an unmodified rebuild is byte-identical to the original."""
    nsec = len(orig.secs)
    sec_entries = [[] for _ in range(nsec)]   # (phys index, bytes)
    phys, logs, names = [], [], []
    for li, (name, rtype, hi, parts, secs) in enumerate(resources):
        logs.append((hi << 24) | (rtype << 16) | len(parts))
        names.append(name)
        first = len(phys)
        for data, s in zip(parts, secs):
            sec_entries[s].append((len(phys), data))
            phys.append([(li << 16) | s, 0, len(data)])
        logs[-1] = (logs[-1], li, first)

    sec_blobs, sec_sizes = [], []
    for s in range(nsec):
        bf, _off, size, csize, cnt = orig.secs[s]
        align = SECTION_ALIGN.get(bf & 0xFF, 16)
        buf = bytearray()
        for pi, data in sec_entries[s]:
            phys[pi][1] = len(buf)
            buf += data
            buf += bytes((-len(buf)) % align)
        buf += bytes((-len(buf)) % align)
        if bytes(buf) == orig.section(s):
            blob = orig.raw[orig.secs[s][1]:orig.secs[s][1] + (csize or size)]
        else:
            blob = zlib.compress(bytes(buf), 9) if csize else bytes(buf)
        sec_blobs.append((blob, len(buf), len(sec_entries[s]), bool(csize)))

    names_blob = bytearray(orig.names_blob)
    name_idx = []
    for n in names:
        if n in orig.name_offset:
            name_idx.append(orig.name_offset[n])
        else:
            name_idx.append(len(names_blob))
            names_blob += n.encode('latin1') + b'\0'

    header_size = 36 + 20 * nsec + 16 * len(phys) + 12 * len(logs) + 4 * len(logs) + len(names_blob)
    out = bytearray(struct.pack('<4s8I', orig.magic, orig.version, orig.flags, len(phys), nsec, len(logs),
                                len(names_blob), len(logs), orig.align))
    data_off = header_size
    for s, (blob, usize, count, compressed) in enumerate(sec_blobs):
        bf, _o, _sz, _cs, cnt = orig.secs[s]
        out += struct.pack('<5I', bf, data_off, usize, len(blob) if compressed else 0,
                           (cnt & 0xFFFF0000) | count)
        data_off += len(blob)
    for pbf, off, size in phys:
        out += struct.pack('<3Ihh', pbf, off, size, 0, 0)
    for bf, li, first in logs:
        out += struct.pack('<3I', bf, li, first)
    out += struct.pack('<%dI' % len(name_idx), *name_idx)
    out += names_blob
    assert len(out) == header_size
    for blob, *_ in sec_blobs:
        out += blob
    with open(out_path, 'wb') as f:
        f.write(out)


# ---------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--game', required=True, help='Dying Light install folder (contains DW\\)')
    ap.add_argument('--anims', required=True, help='folder containing the Bad Blood *_dlbg.anm2 files')
    ap.add_argument('--bb-scripts', help='folder containing Bad Blood\'s compiled %s_part0x.bin '
                                         '(default: AnimationScr next to --anims)' % BB_BANK)
    ap.add_argument('--out', help='output rpack (default: <game>\\DW\\Data\\common_anims_PC.rpack.equip_anim_new)')
    ap.add_argument('--install', action='store_true', help='replace the game rpack with the result')
    ap.add_argument('--no-drop-attack', action='store_true',
                    help='keep DL1\'s stab for the attack while falling instead of Bad Blood\'s drop attack')
    args = ap.parse_args()
    drop_attack = not args.no_drop_attack

    target = os.path.join(args.game, RPACK_REL)
    backup = target + BACKUP_SUFFIX
    out_path = args.out or target + '.equip_anim_new'
    if not os.path.isfile(target):
        fail('rpack not found: ' + target)

    # 1. pristine source
    if not os.path.isfile(backup):
        if b'fpp_knife_show' in open(target, 'rb').read():
            fail('the game rpack is already modified and no backup exists; verify game files in Steam first')
        shutil.copy2(target, backup)
        print('[BACKUP] ' + backup)
    src = RPack(backup)
    print('[SOURCE] %s  (%d resources, sha1 %s)' % (backup, len(src.names), hashlib.sha1(src.raw).hexdigest()[:12]))

    resources = src.resources()
    by_name = {r[0]: i for i, r in enumerate(resources)}

    bb_dir = args.bb_scripts or os.path.join(os.path.dirname(os.path.abspath(args.anims)), 'AnimationScr')
    bb_paths = [os.path.join(bb_dir, '%s_part0%d.bin' % (BB_BANK, i)) for i in (0, 1)]
    for p in bb_paths:
        if not os.path.isfile(p):
            fail('missing Bad Blood bank: ' + p)
    bb_bank = Bank(*[open(p, 'rb').read() for p in bb_paths])
    bb_names = [n.encode() for n in BB_TRACKS]
    for n in bb_names:
        if not bb_bank.track(n):
            fail('Bad Blood bank has no ' + n.decode())
    bb_anims = {bb_bank.track(n)['anm2'].decode() for n in bb_names}

    # 2. banks
    for bank_name in PLAYER_BANKS:
        if bank_name not in by_name:
            fail('bank %s missing from pack' % bank_name)
        i = by_name[bank_name]
        name, rtype, hi, parts, secs = resources[i]
        bank = Bank(parts[0], parts[1])
        if bank.serialize() != (parts[0], parts[1]):
            fail('bank %s does not round-trip; unknown format revision' % bank_name)
        existing = bank.names()
        added = []
        for tname, anim in TRACKS.items():
            key = tname.encode()
            if key in existing:
                # vanilla DL1 already defines it (never triggered without the plugin): keep it
                print('[NATIVE] %s: %s kept' % (bank_name, tname))
                continue
            prefix = tname[:-len('_show')] + '_' if tname.endswith('_show') else None
            if prefix and not any(n.startswith(prefix.encode()) for n in existing):
                print('[SKIP]  %s: no %s* tracks in %s' % (tname, prefix, bank_name))
                continue
            bank.add_track(key, ANIMS[anim])
            added.append(tname)
        copied = []
        for n in bb_names:
            prefix = b'_'.join(n.split(b'_')[:2]) + b'_'
            if not any(x.startswith(prefix) for x in existing):
                print('[SKIP]  %s: no %s* tracks in %s' % (n.decode(), prefix.decode(), bank_name))
                continue
            bank.copy_track(bb_bank, n)
            copied.append(n.decode())
        print('[BB]    %-22s %d tracks copied from Bad Blood' % (bank_name, len(copied)))
        for tname, from_name in CLONED_TRACKS.items():
            if bank.track(tname.encode()) or not bank.track(from_name.encode()):
                print('[SKIP]  %s: not cloned in %s' % (tname, bank_name))
                continue
            bank.clone_track(from_name.encode(), tname.encode())
            print('[CLONE] %-22s %s from %s' % (bank_name, tname, from_name))
        for forbidden in (b'fpp_show', b'fpp_unarmed_show'):
            if forbidden in bank.names():
                fail('%s would catch every weapon' % forbidden.decode())
        p0, p1 = bank.serialize()
        resources[i] = (name, rtype, hi, [p0, p1], secs)
        print('[BANK]  %-22s +%d tracks (%s)' % (bank_name, len(added), ', '.join(added)))

    # 2b. attack while falling: Bad Blood's drop attack on the player and AI sides of the grab
    drop_banks = DROP_BANKS if drop_attack else ()
    for bank_name in drop_banks:
        if bank_name not in by_name:
            fail('bank %s missing from pack' % bank_name)
        i = by_name[bank_name]
        name, rtype, hi, parts, secs = resources[i]
        bank = Bank(parts[0], parts[1])
        if bank.serialize() != (parts[0], parts[1]):
            fail('bank %s does not round-trip; unknown format revision' % bank_name)
        done = []
        for tname, role in DROP_TRACKS.items():
            t = bank.track(tname.encode())
            if not t:
                continue
            start, end = drop_range(role)
            anm2 = DROP_ATTACKER if role.endswith('player') else DROP_VICTIM
            bank.redefine_track(tname.encode(), anm2.encode(), start, end, drop_attack_events(tname, t, role, bb_bank))
            done.append(tname)
        p0, p1 = bank.serialize()
        resources[i] = (name, rtype, hi, [p0, p1], secs)
        print('[DROP]  %-22s %d jump attack tracks now play the drop attack' % (bank_name, len(done)))

    # 3. animations (some tracks reuse animations that already ship with DL1)
    drop_anims = {DROP_ATTACKER, DROP_VICTIM} if drop_attack else set()
    anim_names = sorted(({a['anm2'] for a in ANIMS.values()} | bb_anims | drop_anims) - set(by_name))
    anim_secs = [r[4][0] for r in resources if r[1] == TYPE_ANIMATION]
    anim_sec, anim_hi = anim_secs[0], next(r[2] for r in resources if r[1] == TYPE_ANIMATION)
    for an in anim_names:
        path = os.path.join(args.anims, an + '.anm2')
        if not os.path.isfile(path):
            fail('missing animation: ' + path)
        data = open(path, 'rb').read()
        if data[:4] != b'ANM2' or struct.unpack_from('<H', data, 4)[0] != 0x2A:
            fail(an + ': unexpected ANM2 version')
        resources.append((an, TYPE_ANIMATION, anim_hi, [data], [anim_sec]))
    # keep the compiler's order: builder info, animations (sorted), animation scripts
    order = {255: 0, TYPE_ANIMATION: 1, TYPE_ANIM_SCR: 2}
    resources.sort(key=lambda r: (order.get(r[1], 3), r[0] if r[1] == TYPE_ANIMATION else ''))
    # (stable sort: non-animation resources keep their relative order)
    print('[ANIM]  +%d resources' % len(anim_names))

    # 4. write
    print('[WRITE] %s ...' % out_path)
    write_rpack(src, resources, out_path)

    # 5. verify
    new = RPack(out_path)
    new_res = {r[0]: r for r in new.resources()}
    for name, rtype, hi, parts, secs in src.resources():
        if name in PLAYER_BANKS or name in drop_banks:
            continue
        r = new_res.get(name)
        if r is None or r[1] != rtype or r[3] != parts:
            fail('verification: %s changed' % name)
    for an in anim_names:
        if new_res[an][3][0] != open(os.path.join(args.anims, an + '.anm2'), 'rb').read():
            fail('verification: %s' % an)
    def same_track(o, t, old, b):
        cbs = lambda bank, tr: [bank.cbs[struct.unpack_from('<i', e, 4)[0]] if struct.unpack_from('<i', e, 4)[0] >= 0
                                else None for e in tr['events']]
        return (o['rec'][4:48] == t['rec'][4:48] and o['anm2'] == t['anm2'] and
                [e[:4] + e[8:] for e in o['events']] == [e[:4] + e[8:] for e in t['events']] and
                cbs(old, o) == cbs(b, t))

    drop_names = {n.encode() for n in DROP_TRACKS} if drop_attack else set()
    for bank_name in drop_banks:
        b = Bank(*new_res[bank_name][3])
        old = Bank(*next(r[3] for r in src.resources() if r[0] == bank_name))
        for n in drop_names:
            t = b.track(n)
            if t is None:
                continue
            role = DROP_TRACKS[n.decode()]
            want = (DROP_ATTACKER if role.endswith('player') else DROP_VICTIM).encode()
            if t['anm2'] != want or struct.unpack_from('<ff', t['rec'], 28) != (
                    drop_range(role)):
                fail('verification: %s/%s is not the drop attack' % (bank_name, n.decode()))
        if bank_name in PLAYER_BANKS:
            continue  # the other tracks are checked below
        for o in old.tracks:
            t = b.track(o['name'])
            if o['name'] not in drop_names and (t is None or not same_track(o, t, old, b)):
                fail('verification: original track %s/%s changed' % (bank_name, o['name'].decode()))
        if len(b.tracks) != len(old.tracks):
            fail('verification: %s track count changed' % bank_name)
    for bank_name in PLAYER_BANKS:
        b = Bank(*new_res[bank_name][3])
        for tname, anim in TRACKS.items():
            t = b.track(tname.encode())
            if t and t['anm2'].decode() != ANIMS[anim]['anm2']:
                fail('verification: %s/%s' % (bank_name, tname))
        old = Bank(*next(r[3] for r in src.resources() if r[0] == bank_name))
        oldmap = {t['name']: t for t in old.tracks}
        for n in bb_names:
            t = b.track(n)
            if t and (t['anm2'] != bb_bank.track(n)['anm2'] or t['rec'][8:48] != bb_bank.track(n)['rec'][8:48]):
                fail('verification: %s/%s differs from Bad Blood' % (bank_name, n.decode()))
        for tname, from_name in CLONED_TRACKS.items():
            t, f = b.track(tname.encode()), b.track(from_name.encode())
            if t and (t['anm2'] != f['anm2'] or t['rec'][4:48] != f['rec'][4:48] or
                      [e[:4] + e[8:] for e in t['events']] != [e[:4] + e[8:] for e in f['events']]):
                fail('verification: %s/%s differs from %s' % (bank_name, tname, from_name))
        for t in b.tracks:
            o = oldmap.get(t['name'])
            if t['name'] in bb_names or t['name'] in drop_names:
                continue
            if o and not same_track(o, t, old, b):
                fail('verification: original track %s/%s changed' % (bank_name, t['name'].decode()))
    print('[OK]    %d resources verified, %d bytes' % (len(new.names), os.path.getsize(out_path)))

    if args.install:
        os.replace(out_path, target)
        print('[INSTALL] ' + target)
        print('          restore vanilla by copying %s over it (or Steam > Verify integrity)' % os.path.basename(backup))


if __name__ == '__main__':
    main()
