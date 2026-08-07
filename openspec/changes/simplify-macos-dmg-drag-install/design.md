## Context

The current macOS disk image presents `ACECode.app` on the left, a custom `Install ACECode.app` helper on the right, and a visible instructions file below them. That helper installs into `~/Applications` so the existing native updater can rely on a writable per-user destination. The result does not match the macOS convention users expect from a drag-install DMG and makes the right-hand item look like an unexpected second application.

This change crosses packaging, CMake, the release workflow, native update path validation, tests, and release documentation. The signed application and DMG must continue through the existing Developer ID and notarization pipeline. Path validation must remain strict because the native updater replaces an installed application bundle on disk.

## Goals / Non-Goals

**Goals:**

- Present the standard two-item Finder layout: `ACECode.app` on the left and an `Applications` link on the right.
- Remove the helper installer and visible instructions file from the public DMG.
- Keep the DMG background simple enough that the app, arrow, and destination are immediately clear.
- Permit secure native self-update for applications installed at either `~/Applications/ACECode.app` or `/Applications/ACECode.app`.
- Preserve signing, notarization, stapling, deterministic layout checks, and existing per-user installations.

**Non-Goals:**

- Automatically move existing per-user installations into `/Applications`.
- Add a privileged helper or request administrator credentials during self-update.
- Change the update manifest, release server, signing identity, or non-macOS packages.
- Support arbitrary renamed bundles or arbitrary installation directories.

## Decisions

### Use a real Finder alias target implemented as a symlink to `/Applications`

The DMG creation script will create an `Applications` symlink whose target is exactly `/Applications`. Finder renders that item with the familiar Applications folder icon and accepts a direct drag of `ACECode.app` onto it. This is the conventional layout used by drag-install disk images and is clearer than an installer application.

An alias file or custom launcher was considered, but both add packaging complexity and provide less transparent destination semantics than the standard symlink.

### Limit the visible DMG payload to two installation items

The mounted image will expose `ACECode.app` and `Applications`, with the background and volume metadata remaining hidden. `Install ACECode.app` and the visible instructions text file will not be copied. The Finder layout script will position only the two visible items and the background will contain only restrained branding and a directional arrow.

Keeping an instructions file was considered, but the drag target already communicates the only required action and the user explicitly prefers an uncluttered image.

### Remove the custom installer from the release build path

The release workflow and desktop CMake configuration will stop building, signing, symbol-uploading, and passing the custom current-user installer into DMG creation. Installer-only source and artwork can be removed once no production path references them. The main desktop application remains signed and notarized exactly as before.

Leaving a dormant release target was considered, but it increases maintenance and makes it easier for the obsolete helper to return accidentally.

### Allow exactly two native self-update destinations

The updater will accept only these canonical bundle locations:

- `~/Applications/ACECode.app`, retained for existing installations.
- `/Applications/ACECode.app`, used by new standard drag installs.

For either location, the updater will require the parent and installed bundle to be real directories rather than symlinks, resolve both paths, and confirm that the resolved destination is exactly the expected canonical bundle. It will also require write and traversal access to the destination parent before starting replacement. All staging, backup, and lock paths remain siblings of the installed bundle so the existing same-volume atomic replacement strategy continues to apply.

Supporting arbitrary writable destinations was considered, but it would broaden the updater's filesystem mutation surface and weaken the current security boundary.

### Fail safely when `/Applications` is not writable

No privileged helper will be introduced. If the current process cannot modify `/Applications`, the updater will stop before mutating the installed bundle and return an actionable error that directs the user to install the downloaded DMG manually. This keeps the update flow auditable and avoids adding a privileged component solely for replacement.

## Risks / Trade-offs

- **Some users cannot write to `/Applications` during self-update** → Check permissions before mutation and direct those users to drag the newer app from the DMG with Finder, which can request authorization.
- **Users may keep both per-user and system copies** → Update only the bundle that is actually running; do not move or delete the other copy automatically.
- **Finder can retain stale window metadata while rebuilding a DMG** → Continue creating a fresh writable image, apply layout with bounded retries, and verify mounted output in targeted packaging checks.
- **Removing the helper changes an established packaging contract** → Update release workflow assertions and script contract tests in the same change so obsolete arguments or payloads fail validation.

## Migration Plan

1. Ship the simplified DMG and updated native path policy together.
2. Existing `~/Applications/ACECode.app` copies continue using their current update path without relocation.
3. New users drag the app into `/Applications`; later updates target that launched system copy when permissions allow.
4. If release validation fails, restore the previous packaging workflow while retaining the compatible two-destination updater policy, which is backward compatible.

## Open Questions

None.
