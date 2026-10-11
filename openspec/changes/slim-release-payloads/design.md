## Context

See proposal.md for measured baselines. `acecode_embed_assets.cmake` currently emits raw arrays and `static_assets.cpp` returns them; the route already handles cache headers. `agent_office_pet.html` owns the actual pixel body, accessories, palette, seeded appearance and animation. Session metadata already persists expert/member IDs, but the office projection drops these fields. macOS desktop and flat CLI share one manifest target; existing extractors reject ZIP symlinks, and old flat validators require a root `acecode` file.

## Goals / Non-Goals

**Goals:** Retain complete offline features, URLs, custom expert resources, session semantics and old updater compatibility while reducing packaged data. Reuse the office's existing pixel art exactly.

**Non-Goals:** Frontend feature removal, dependency/language reductions, layout redesign, deleting developer build caches, publishing a release or rewriting tags. Manual macOS portable archive layout is outside this update-ZIP change.

## Decisions

1. Generate gzip byte arrays using a Python standard-library generator called by CMake; timestamp is fixed, files sorted, binary reads preserved. Keep uncompressed form only where smaller. Carry stored size, decoded size and encoding metadata. Negotiate gzip at HTTP boundary; cache immutable decoded data for clients requiring identity encoding. Existing filesystem sources remain live and uncompressed. Reuse zlib with an explicit native dependency. Keep reconfigure dependencies for existing-file changes and generator changes.
2. Extract pixel character templates, palette and deterministic look selection into one small JavaScript asset consumable both by React and the self-contained pet page. Generate portrait pixels from those templates; do not introduce new image artwork or a second appearance algorithm. Use effective expert identity from structured `expert_id`/`expert_member_id`; root team maps to its lead, referenced members map to their expert IDs. Keep custom images intact and use pixel defaults when no custom image is configured. Replace managed built-in portrait declarations and remove their obsolete PNGs, bumping the seed bundle with the standard reconciliation machinery.
3. Add a tiny native macOS CLI forwarder, stored inside `ACECode.app/Contents/MacOS` so the existing nested-code signing and app notarization cover it. The update ZIP copies just that small forwarder to root as `acecode`; the large daemon and resources exist only inside the app. The wrapper resolves its real executable location and `exec`s the bundled daemon, preserving arguments, signals and exit status. This retains the old flat validator/application path and makes later invocations use the existing signed-app update path. An app-only ZIP would break old CLI clients; symlink ZIP entries are rejected by the existing extractor. The tiny forwarder duplicate is intentional compatibility overhead, not a duplicate runtime.
4. Keep edits scoped to current checkout. Capture current file baselines and preserve concurrent skill-scan work, especially shared API documentation and generated i18n content. No release side effects.

## Risks / Trade-offs

- [Bad encoded bytes cause a blank UI] -> binary round-trip fixtures including control bytes, deterministic regeneration tests, HTTP gzip/identity checks and browser startup verification.
- [HTTP caches mix encodings] -> Vary on Accept-Encoding for compressed resources and explicit q=0 handling.
- [Avatar drift between surfaces] -> one shared character source and expert binding tests across catalog, subagents and office snapshots.
- [Seed upgrade overwrites custom portraits] -> existing managed-hash reconciliation, fresh/old/custom profile tests, no blanket user-directory cleanup.
- [CLI wrapper loses path or signing compatibility] -> argument/space/symlink/exit probes, old-validator package tests, nested-code signature verification; native macOS checks remain explicitly recorded if not available on this host.

## Migration Plan

The source change takes effect on the next normal package build. Existing desktop updates use the unchanged signed app installer. Existing flat CLI installs receive a small root entry plus the app; unrelated old files remain untouched. Custom expert packages remain preserved by seed reconciliation. Rollback is a normal previous-version package and does not require manifest schema or target-name migration.

## Validation record (2026-10-10)

- Generator/native-page script tests: 3 passed, including binary control bytes, deterministic gzip, CMake incremental rebuild, and exact shared-office script inlining.
- Web suite: 3087 existing `[pass]` records plus 3 new Node tests passed; production build and ES2020 regex compatibility check passed.
- Browser: 9 pixel-identity checks; existing office suite passed 70 checks with the Windows bridge fixture and 70 with the macOS bridge fixture; opt-in/welcome/polling/preview suite passed. Real catalog, subagent and office components were rendered in Edge; these are browser fixtures, not an installed Desktop update.
- macOS forwarder/package: 3 tests compiled and executed the real POSIX launcher on WSL, exercised spaces, PATH/symlinks, exact arguments and exit codes, verified one runtime/resource tree, and rejected missing/oversized forwarding entries. Portable package tests 4 passed; release-asset tests 6 passed; release shell contracts passed against an LF-normalized temporary fixture (the Windows checkout uses CRLF). Native macOS signature/notarization and `_NSGetExecutablePath` execution are not available on this Windows host; the new test is registered in the macOS CI job.
- Layer, source-size, ownership, include, doc-path and migration-map gates passed; the new identity helper is separate from the size-capped registry implementation.
- Final native-generation measurement after build-version substitution: 317 files, 13,800,341 raw bytes -> 4,480,962 stored bytes (67.53% reduction; 8.89 MiB saved). Replaced default PNGs: 3,853,891 bytes removed. Seed version is `2026-10-10.1`; no built-in expert PNG remains.
- Windows `acecode`, `acecode-desktop`, and `acecode_unit_tests` built successfully in `build/refactor-phase1-windows`. The first long build predated the added source-file inventory and failed at link; CMake regeneration and the final 27-step incremental build passed.
- Focused native suite: 141 executed, 0 skipped, 0 failed (19.2 s), including `DesktopOfficeHttp.ExpertMemberIdentitySurvivesOfficeProjection`, both `PixelPortraitUpgrade*` tests, `UpgradeApply.LegacyMacFlatInstallAcceptsOneAppAndSmallForwarder`, packaged seed manifest hashes, and gzip negotiation/content/cache tests.
- Real staged daemon: 8 HTTP checks passed for gzip, identity, explicit gzip quality zero, concurrent fallback, SPA fallback and versioned SVG caching. Served HTML is 12,875,655 bytes decoded / 3,866,414 bytes gzip; decoded SHA-256 is `53833d3fc42a01a61e99829dfaf301490b5d2bd79efb26156789158d06dc95a8` and matches current dist with version substitution. Edge successfully started the actual compressed frontend. Isolated test processes/profiles were cleaned up; the existing installed Desktop processes were left running.
- Browser test maintenance: the existing office preference test now reads the label span instead of concatenating shortcut badges; no production menu behavior was changed. The HTTP browser harness explicitly writes UTF-8 to Node stdin on Windows.
