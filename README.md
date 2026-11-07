# SCSSoulCharge

An add-on for [Spellcasting Staff NG](https://www.nexusmods.com/skyrimspecialedition/mods/196207) 2.1.1 (SCS, by hamkidna, MIT) for Skyrim SE 1.5.97 / AE 1.6.1170 / AE 1.7.104.

- Vanilla and mod staves become SCS catalysts: the DLL gives them the SCS keywords at load time.
- A staff's own enchantment becomes a "charge spell" in the magic menu, cast through SCS and paid from the staff's soul gem charge (fire-and-forget per shot, concentration per second; refused when the charge cannot pay).
- While charging, the staff tip shows the bound spell's casting art.

Status: development build, tested in game on 1.6.1170 only. The test configuration converts only the staves listed in `SCSSoulCharge.ini` (`bOnlyListedStaves=1`).

## Layout
- `src/` SKSE plugin (CommonLibSSE-NG = alandtse/CommonLibVR 10.1.0, cloned into `extern/`, not tracked), `src/core/` game-independent rules with offline tests in `tests/`
- `tools/build_esp.py` builds `SCSSoulCharge.esp` (1024 placeholder charge spells + a vanilla test staff)
- `dist/` INI shipped with the plugin
- `docs/` design, phase 0 findings, implementation plan with test results, in-game test procedure
- `reference/scs-2.1.1/` SCS 2.1.1 source for reference (MIT, copyright hamkidna), not modified

## Build
`build.cmd` (Visual Studio 18, vcpkg at `C:\Dev\vcpkg`, Microsoft Detours from vcpkg). Output goes to `D:\DEV\build\SCSSoulCharge\release`.

## License
The plugin links CommonLibVR (GPL-3.0 with modding exceptions), so it is distributed under GPL-3.0. `reference/scs-2.1.1` keeps its own MIT license.
