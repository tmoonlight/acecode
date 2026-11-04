# Design

## Context

See proposal.md for motivation. `load_and_apply` is shared by daemon, TUI and headless before tool registration. `load_settings` is also used by the read-only REST GET. Missing files currently return disabled OpenCode seed mappings. PUT parses a new settings value rather than round-tripping private fields. Compatible generated paths already support both underscore and legacy hyphen filenames.

## Goals / Non-Goals

**Goals:** Keep migration at the shared startup boundary, keep ordinary reads free of writes, and make persisted completion independent of frontend metadata.

**Non-Goals:** Change internal tool identifiers, change the built-in reset seed, add UI text, or publish a release.

## Decisions

- Add an optional boolean `write_file_default_applied` to the settings model/file. Absence means pending; a malformed marker is a load error. Keep format version 1 because older readers already ignore unknown fields.
- Startup constructs a candidate only after a successful read. If disabled, enable it and upsert just `file_write -> write_file`. Already-enabled settings only receive the completion marker. Validate and atomically persist before publishing the candidate.
- Every explicit `save_settings` call writes completion as true, even if a REST client omits or supplies a false marker. Saving settings is an explicit user choice and must not be reversed on the next startup. Serialization itself retains the model's marker for predictable round trips.
- Keep the marker in the same atomic file as settings, avoiding two-file completion races. Do not infer completion from application version, enabled state, or the alias itself.
- Preserve the read-only loader and disabled fallback. Filesystem status/read errors must be distinguished from a missing file so migration cannot overwrite inaccessible data.

## Risks / Trade-offs

- A custom mapping may already target `write_file` -> validation rejects the migration, logs the conflict and retains the user's file.
- Manually deleting the settings file or completion field resets migration state -> normal UI saves always record completion; manual removal is treated as resetting settings.
- Existing cross-process settings saves have last-writer semantics -> this change retains that storage model; no new locking primitive is introduced.
- Old binaries can drop unknown metadata when saving -> rollback can reset completion; subsequent use of the new version can migrate again.

## Migration Plan

Ship with the next ordinary release. The first startup per data directory migrates automatically; no release version bump is required in this patch. After migration, normal settings edits including disabling are authoritative. Reverting code does not revert the saved mapping, which remains valid for older readers.
