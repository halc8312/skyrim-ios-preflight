# Skyrim iOS development

- Work independently of the existing Unity project and Steam installation. Never modify original executables, Data, saves, mods, or Steam settings.
- Work on `codex/*` branches. Do not publish or create a remote without user authorization. Keep game inputs, derived code/data, build products, device identifiers and signing material under ignored private paths.
- Use reproducible static x86-to-C-to-ARM64 translation, initially Skyrim 2011 without mods. Do not treat SE, macOS, simulator, linking, or a diagnostic probe as iPhone gameplay success.
- Unsupported instructions/APIs and unresolved calls must remain explicit failures with diagnostic context, never hidden by success-shaped stubs.
- Preserve original failures, build logs, source versions and signing/distribution evidence. Do not delete ignored files as cleanup.
- No unapproved paid resources, dependencies, model downloads, public distribution, or quality reductions.
- Delegate useful independent work with concise context. Reuse verified results, run tests proportional to actual risk, and keep STATUS.md accurate about evidence and remaining blockers.
