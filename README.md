# SCSSoulCharge

SKSE plugin for Skyrim SE **1.5.97** and AE **1.6.317 to 1.7.x** (one DLL; VR not supported): bind a known spell to one staff item. While that staff is held, it casts the
spell instead of its own enchantment, paid with magicka at the spell's own cost, with the spell's range, tip art and
skill training. Both hands are supported. Nexus page: 198580.

- Hold a staff, open the magic menu, pick a spell for that hand: it is bound to that staff (picking it again clears it).
- The staff is renamed `<name> [<spell>]`; the bound spell's name ends with ` ★R` / ` ★L` (both when one spell is on
  both staves) while its staff is held.
- Bindings live in the SKSE co-save, per item (record + `ExtraUniqueID`).
- Notices are translatable (`SCSSoulCharge_Messages.ini`, UTF-16 LE); a Korean file is in `dist-optional/Korean`.
- Other plugins: exported functions and an `'SCST'` SKSE message per staff cast, see `src/api.h` (API 4).

Status: 1.0.1. Proven in game on 1.6.1170 (self-tests `SELFTEST PASS/FAIL` in `SCSSoulCharge.log` with
`[SelfTest] bEnabled=1`); design and test history in `docs/design-standalone.md`.

Game versions: 1.6.1170 is tested in game. 1.7.104 is checked offline against its executable
(`tools/runtime_check_harness.py`). Other AE builds and SE 1.5.97 rely on start-up checks: every engine address is
looked up for the running build and checked in the running game's code (`src/runtime_check.cpp`); a failed check turns
the mod off with a notice and a `T-RUNTIME FAIL` line in `SCSSoulCharge.log` instead of calling a wrong function. The SE
addresses come from meh321's 1.5.97-to-1.6.318 match list (Address Library). SE needs SKSE 2.0.18 or later.

## How it works
A runtime `EnchantmentItem` copy of the bound spell goes into the hand slot (`selectedSpells`), written the way the
engine's setter does it (preload 11303 / SE 11195, release 11306 / SE 11198), from a `Main::Update` frame hook (main thread). The copy answers
range, flags and skill usage with the spell's values, its cost is the spell's cost (`CalculateCost` 11213) paid from
magicka (resource lookup 33817), its effects are re-pointed at the spell's effects as they land (`MagicTarget::AddTarget`)
so lasting effects survive a restart, and the tip art is swapped with DetachCastingArt 34152 / SetCastingArt 34150
(SE 33371 / 33369).
The magic menu's spell equip call (37952/38908 +0xD7) is chained after Spellcasting Staff NG's own hook.

## Layout
- `src/` plugin; `src/core/` game-independent rules with offline tests in `tests/` (`core_tests`)
- `dist/` INI shipped with the plugin; `dist-optional/Korean/` Korean notices
- `docs/` design and test history
- `reference/scs-2.1.1/` Spellcasting Staff NG 2.1.1 source for reference (MIT, copyright hamkidna), not modified
- `extern/CommonLibSSE-NG` (cloned, not tracked)

## Build
`build.cmd` (Visual Studio, vcpkg, Microsoft Detours from vcpkg). Output: `D:\DEV\build\SCSSoulCharge\release`.

## Credits
- Spellcasting Staff NG by hamkidna (MIT): reference for the magic-menu equip hook and the staff keyword.
- Immersive Staves Tweaks by Czy2007: reference for the staff cost function it hooks.
- CommonLibSSE-NG, Microsoft Detours (MIT), spdlog (MIT).

## License
CommonLibSSE-NG is GPL-3.0-or-later with its modding and linking exceptions, so this plugin is distributed under
GPL-3.0 (`LICENSE`). `reference/scs-2.1.1` keeps its own MIT license.
