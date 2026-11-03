## Context

TurnFileList already corrects historical worktree paths. DesktopContextMenu owns capture-phase context menus, path copying, Explorer opening, runtime filtering and keyboard behavior. The daemon owns authenticated preview resolution and a native save picker used by session export. Binary preview currently permits only known preview MIME types and caps reads at 20 MiB; text reads have their own binary/size checks.

## Goals / Non-Goals

Reuse these owners rather than add per-card listeners, clipboard code, native dialogs, or path joining. No new viewer, card chrome, persisted state, or unrelated menu changes.

## Decisions

- Mark each row with its corrected open path and session cwd. Resolve an absolute target using the existing file-link resolver. Give the card target priority over surrounding message/file-link targets.
- Add only Save file as and Copy file contents action IDs. Reuse Copy absolute path and Open in Explorer with the reference's labels. Keep all four in one plain-text group.
- Add authenticated `POST /api/files/export` using `resolve_preview_path` and the injected native picker. Copy original bytes after validating the source and the picker destination; cancellation is a successful no-op. Never accept a destination from the request.
- Extend the existing binary reader with `download=1` to permit arbitrary file types as attachments, keeping the 20 MiB read limit and path/auth policy. Extract the existing Mermaid Blob downloader into a shared helper and retain its existing export alias. Browser Save as reuses it; desktop uses the export endpoint.
- Copy text via `api.readFile` and the existing clipboard/toast helper. Read on action, not on render; do not copy recorded hunks or fetch contents speculatively.
- Keep session/theme picker defaults unchanged. The shared native picker receives an optional all-files mode through a separately injected generic-file callback. Empty text copying opts into the shared clipboard helper's empty-value support without changing other callers.

## Risks / Trade-offs

- Missing/deleted files remain in historical cards: report existing file errors; keep recorded diffs usable.
- Browser downloads retain the existing 20 MiB bound; native copies do not buffer the file in JavaScript.
- Browser Save as follows browser download settings; desktop uses the native picker. Ordinary browsers omit Explorer under the existing policy.
- Source files can change before copying: actions deliberately use current disk contents.

## Validation

Exercise target resolution, menu priority/order, native/browser dispatch, cancellation, original bytes, unsupported text, and API failures. Run full Web tests/build, focused native HTTP tests, OpenSpec validation, and browser checks of the actual components at desktop and narrow widths in both themes.

### Results (2026-11-02)

- Full `pnpm test` passed (3,481 `[pass]` assertions plus the suite's Node test summaries); `pnpm build` and regex compatibility checks passed. Existing bundle-size warnings remain.
- Windows MSVC Release `acecode_unit_tests` build passed. Thirteen focused HTTP/native-string tests passed, including all four new download/export tests and existing session-export/PDF/Office regressions.
- The source test file changed during the first compilation; an explicit source timestamp refresh and rebuild ensured the new test names were included before recording the 13-test result.
- Browser checks used production TurnFileList/DesktopContextMenu/CSS with fixture data and mocked API/desktop bridges. Covered all card hit areas, exact four-item order, absolute/corrected paths, current text/empty text, binary errors, cancellation without success toast, Explorer dispatch, independent preview/diff clicks, folding, keyboard navigation/dismissal, and browser binary downloads preserving bytes.
- Inspected 1280x800 Chinese/light and 390x700 English/dark screenshots in `verification/`; no page errors or horizontal overflow. The design detector returned no findings. A separate finish review returned `ship` for the UI/DRY scope.
- Strict OpenSpec validation and `git diff --check` passed. Temporary browser fixture files and the test server were cleaned up.
- Actual native dialog/OS clipboard/Explorer interaction was not exercised in a running Desktop shell. macOS/Linux changes were source-reviewed, not built here. No commit, push, release, or desktop restart was performed.
