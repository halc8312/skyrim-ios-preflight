# Classic MOD management — first implementation design

Status: design only, 2026-11-14. The next build does not install a MOD manager or enable any proposed MOD. Target: the pinned 2011 PC executable, Skyrim.esm and Update.esm. This is separate from engine performance work.

## First usable scope

Import an explicitly selected, locally owned classic data MOD; inspect its files and master dependencies; enable, disable, and order it before launching. Preserve the original game directory. Start with authored fixtures and ordinary ESP/ESM, compatible BSA and approved loose data. No automatic Nexus downloads, credentials, installer execution, Windows DLL/SKSE loading, ESL, or assumed Special Edition compatibility.

Archive import initially accepts ZIP and an unpacked directory. Other archive formats must be explicitly supported later, not silently partially imported. Each package has a content digest, display name, source reference if supplied, file inventory, plugin metadata and declared requirements. Files enter a fresh staging directory; duplicate normalized paths, absolute/traversal paths, links, case collisions, excessive file counts or decompressed sizes, and executable payloads refuse the import. User-supplied names never become filesystem paths directly. BSA compatibility requires actual header/version inspection; an extension is insufficient.

## Profile and data ownership

Keep installed packages in separate immutable directories under the app's Documents area. A profile identifies an ordered package selection, plugin order, INI choices, and a dedicated save directory. A published launch manifest binds the exact package versions and selected files. The existing host already passes a game directory and profile directory to the runtime; introduce resolution at that boundary rather than editing the original Data tree.

The first implementation builds a resolved file index while the game is stopped. Loose-file lookup and enumeration must use the same case-insensitive winner map, including directories, deletions and negative lookups. Original files remain the fallback. Plugin discovery and BSA opening use the same frozen profile. Reuse the game's BSA loader; do not extract or flatten BSA contents to guess their priority. Existing runtime path translation, archive loading, directory enumeration and access caching all need integration review before this is enabled.

Plugin order and loose-file overwrite order are distinct. Show both, and show the actual winner for a file conflict. Use the classic game's enable/load-order format, verified against a small authored master/dependent fixture; never copy SE's format based only on the filename. Master dependencies form an acyclic graph: missing masters, duplicate plugin names, version mismatch or impossible order blocks launch. Preserve engine ordering requirements and reject an unsupported plugin count. A dependency scan establishes loadability constraints, not gameplay safety.

Publish a complete new profile generation only after validation. An atomic manifest replacement is the commit point. On interruption, retain the prior committed profile; incomplete staging is not selectable. Pin the manifest and package references for the entire game process. Imports may be staged while playing, but enable/disable/order changes apply only on the next launch. No mid-session hot reload or removal of packages still used by a running session.

## Saves and UI

Keep the current vanilla profile and saves intact. Creating a modded profile offers an explicit copy of a chosen save; it never moves the source or duplicates all game assets. Associate saves with the active package digest/order in a separate sidecar. Missing or changed packages produce a clear mismatch notice before loading. Disabling a MOD does not undo changes already baked into a save; retain access to the previous profile and save rather than promise reversible gameplay changes.

Settings defaults to English with the existing Japanese language option. A minimal Mods page provides Import, Enabled, Order, Dependencies and Conflicts, and shows “Applies after restart.” A profile picker exposes Vanilla and user-created profiles. Script-bearing packages and record edits need explicit compatibility review in the initial allowlist. FPS claims are not inferred from a MOD's name or download count.

## Acceptance work before implementation is admitted

1. Authored archives cover path escape, duplicate/case aliases, links, unsupported formats, byte/file-count limits, corrupt headers and interrupted import. No game assets belong in public tests.
2. Authored plugins prove master order, disabled dependencies, cyclic references, plugin cap and exact classic enable/order roundtrip.
3. Compare loose and BSA asset resolution, enumeration and cache invalidation across two profiles. Test collisions with original data and package removal while an old session remains pinned.
4. Crash each publication step: either the complete old or complete new generation must remain launchable, without editing original data or saves.
5. On a device, first launch an authored harmless data MOD; then test a separately reviewed real MOD with a copied save. Confirm load, save, relaunch, profile switching and asset correctness. No cloud uploads of game data or save contents.

## Initial proposal decisions

- SPO classic 1.6: dependencies fit the base-game set, but the actual package contains unrelated placement/lighting/ownership edits. Do not enable the unmodified package as a transparent engine optimization.
- Realistic Ragdolls and Force: optional behavior change; no measured performance gain.
- Dead NPC Body Cleaner Remover: the suggested SE version is incompatible; the classic author's requirements include all three DLC and SKSE. Excluded from the current base-game setup.
- Possessive Corpses: a 2012 fix for accumulated weapons/remains/nirnroot effects. Need exact package and comparison with the pinned 1.9.32 scripts/records before deciding relevance. No automatic adoption or claim of modern-port stability.

This design is the completion of the agreed minimum-design item, not an implemented compatibility layer. The next build contains only reviewed host changes and leaves the current vanilla content selection intact.
