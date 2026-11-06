# SpellCastingStaff 2.1.1

Release 2.1.1 includes persistent spell assignments per staff base form and immediate remembered-name previews in paused Inventory and Favorites menus. SKSE plugin version: 2.1.1.0.

## Behavior

- Assign a learned spell while holding a custom staff. Select the same spell again to clear that staff assignment; select again to reassign.
- Unequipping restores the original name and preserves the last assignment. Reequipping silently restores it.
- Each staff kind/tier remembers its own spell. Copies sharing one base WEAP form share the assignment. Memory is stored in the SKSE co-save; retain the .skse file with your save.
- Paused-menu reequipping previews names only. Gameplay reconciliation resumes after unpausing. Manual spell selection retains its existing binding path.
- One custom staff is supported at a time. Shared ENCH metadata follows the equipped staff; stored spells are not copied into the shared effects.

## Supported runtimes

SE 1.5.97.0, Steam AE 1.6.1170.0 and Steam 1.7.104.0. Other builds, GOG and VR are unsupported. Use matching SKSE and Address Library.

## Build and install

Use VS2022/MSVC 14.44, vcpkg manifest mode and x64-windows-static-md. Retain the bundled vendor/CommonLibSSE-NG backport. With VCPKG_ROOT configured in your developer command prompt, run `cmake --preset release`, then `cmake --build build/release`.

Install the compiled SpellCastingStaff.dll under Data/SKSE/Plugins. The startup banner must read SpellCastingStaff v2.1.1. Keep your existing mod assets; this source package contains no DLL, meshes, textures, game executable or Address Library database. Reference/SpellCastingStaff.esp is unchanged and English-localized; do not blindly overwrite a customized ESP.

Choose one configuration: config/SpellCastingStaff.ini (English), or config/SpellCastingStaff_Korean.ini renamed to SpellCastingStaff.ini (Korean). Install as Data/SKSE/Plugins/SpellCastingStaff.ini and restart the game after editing. INI language does not translate ESP records or diagnostic logs.

Logs are in the actual Documents/My Games/Skyrim Special Edition/SKSE folder, named SpellCastingStaff_<runtime>.log. bVerboseLog=0 is the normal default; use 1 for reports. iDiagnosticHotkey=0 disables the shortcut; 64 selects F6. Diagnostics do not toggle gameplay perk effects.

## Validation and limitations

The preceding functional build was user-tested on SE 1.5.97: Favorites renaming now works, and supplied logs show independent remembered assignments and matching casts. Host regression results for this release are in docs/TEST_RESULTS_2.1.1.txt. A full Windows build and in-game retest of 2.1.1 remain outstanding; automated mocks do not certify engine/UI behavior or crash-free operation.

Mods repurposing Variable08 may conflict. Required menu-hook failure disables staff functionality with a compatibility message. Optional bound restoration is skipped if patch validation fails. Unknown E9/menu-hook replacements are not blindly chained. All mod combinations and long sessions are not certified.

See README_KO.md, CHANGELOG.md and docs/REVIEW_2.1.1_KO.md. Historical revisions are under docs/history. The optional undocumented MSVC SCS_TRIM_RELEASE_FILE_PATHS option stays off by default; verify the resulting DLL if enabling it. Vendor licenses and the CommonLib backport diff are retained.

## Review fixes included in 2.1.1

The user reports successful AE 1.7.104 testing; the new preview path remains enabled on AE by request. Its 1.6.1170 in-game verification remains outstanding. This follow-up has not been Windows-built or game-tested here.

The loaded-validation flag now uses atomic store/exchange; this is not a guarantee of whole-controller thread safety. Effect ownership rejects non-kOther/non-Fire-and-Forget effects before resolving the caster. The omitted MIT permission phrase has been restored. No debounce window, memory-disable option, INI or ESP changes were added. The startup banner includes.

For a public build using the optional undocumented path trimming flag, configure with `cmake --preset release -DSCS_TRIM_RELEASE_FILE_PATHS=ON`, then `cmake --build build/release --clean-first`. Inspect the actual DLL for remaining local paths; do not assume all paths are removed. Avoid including private PDB files. Nexus permission settings have not been inspected or changed.
