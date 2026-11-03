# Dying Light: Bad Blood animations and moves

An ASI plugin plus a rebuilt animation pack that bring Dying Light: Bad Blood's equip animations and special moves into Dying Light (the first game, Steam build 24076292, July 15, 2026 update).

> **Just want to play?** Download `DyingLight_BadBlood_Mod.zip` from the Releases page and follow the README inside. This repository is the source code.

## What it does

- Bad Blood's weapon equip animations, with instant weapon switching.
- Bad Blood attack animations (knife charged attack, heavy weapon swings, throws, ground pound).
- Charged attack, whirlwind, head stomp, attack while falling and ground pound work without the skills.
- Bad Blood's PvP dive attack, adapted to zombies: slowed fall, head destroyed on impact, kick, camera that follows the target.

## Repository layout

| Path | Content |
|---|---|
| `plugin/` | `equip_anim.asi` source (C++, Visual Studio, MinHook). `dllmain.cpp` finds every game function by signature and hooks it; `equip_anim.ini` holds the settings. |
| `tools/build_equip_rpack.py` | Builds the modified `common_anims_PC.rpack` from the game's original one and Bad Blood's unpacked animations (Python 3, standard library only). |
| `docs/DOCUMENTATION.md` | Everything learned about the formats and the game code: animation banks, rpack layout, addresses, how each move works, version history. |
| `licences/` | Licenses of MinHook and Ultimate ASI Loader. |

## Building

**Plugin** (Visual Studio 2022 or later, v145 toolset, x64):

```
MSBuild plugin\equip_anim.vcxproj -p:Configuration=Release -p:Platform=x64
```

The result is `plugin\x64\Release\equip_anim.asi`. It needs `MinHook.x64.dll` and an ASI loader (Ultimate ASI Loader as `dinput8.dll`) next to `DyingLightGame.exe`.

**Animation pack** (you need your own copy of both games):

```
python tools\build_equip_rpack.py --game "C:\...\Dying Light" --anims "C:\...\dlbb\unpack_dlbb\Animation" --install
```

`--anims` is the folder with Bad Blood's unpacked `.anm2` files, and the compiled animation scripts must be in `AnimationScr` next to it. The script keeps a backup of the original pack (`common_anims_PC.rpack.equip_anim_backup`) and always builds from it. `--no-drop-attack` keeps Dying Light's stab instead of Bad Blood's dive attack.

## Not included

Game files (the `.rpack`, the `.anm2` animations, the game DLLs) belong to Techland and are not in this repository. After an update of Dying Light the signatures may need to be redone: see `docs/DOCUMENTATION.md`.

## License

No license has been chosen yet: add a `LICENSE` file before publishing if you want others to reuse the code.
