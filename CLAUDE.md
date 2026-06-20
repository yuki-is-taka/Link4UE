# Link4UE

Ableton Link + Link Audio integration for Unreal Engine: shared tempo/beat/phase, transport (start/stop) sync, and real-time audio channel streaming across Link peers on the local network.

Git-managed (independent repo). Use `git`, not `p4`. Contains Ableton Link as a submodule (which itself nests ASIO) — initialize with `--recursive`; do NOT modify the submodule. GPLv2+ licensed. Never commit without explicit user approval.

## Notes
- Modules: `Link4UE`, `Link4UEEditor`.
- Settings live on `ULink4UESubsystem` (Engine Subsystem) as `UPROPERTY(Config)`, stored per-user in `EditorPerProjectUserSettings.ini` (not version-controlled). Blueprint mutators apply at runtime but do not persist unless `SaveConfig()` is called.
- `SetTempo`/`SetQuantum`/`SetIsPlaying`/`RequestBeatAtTime` are session-only (not persisted).

## Documentation
Read [`Docs/INDEX.md`](Docs/INDEX.md) before non-trivial work. Decisions (immutable): `Docs/decisions/`. This repo follows the project doc-system convention.
