# Tasks

## 1. Recommendation generation

- [x] 1.1 Add bounded transcript context, completion validation and tool-free generation; cover completed, aborted, pending, malformed and oversized data with native tests.
- [x] 1.2 Add the authenticated session endpoint with pre/post completion checks and document its API; verify focused native build and endpoint behavior.

## 2. Composer behavior

- [x] 2.1 Add the shared completion-scoped request controller and both transcript-state triggers; test duplicate events, revisits, new turns, failures and stale responses.
- [x] 2.2 Render muted recommendations and implement Tab acceptance/Escape dismissal with draft, attachment, IME and menu protection; verify actual composer browser interactions and themes.

## 3. Integrated verification

- [x] 3.1 Run complete frontend tests/build, focused native tests and integrated chat browser checks; record results and limitations.
- [x] 3.2 Run strict OpenSpec validation and diff review; verify only intended additions and edits are included.

## Verification (2026-11-01)

- Complete `pnpm test` passed (3473 `[pass]` records); `pnpm build` and bundled regex compatibility checks passed.
- MSVC Release `acecode_unit_tests` built; all 12 `FollowUp*` tests passed, including real authenticated HTTP routing and a local model fixture that verifies tool-free generation, unchanged transcript and stale-result rejection.
- `node scripts/test-follow-up-prompts.mjs` passed with the production transcript reducer, hook, InputBar/Slate and actual ChatView: first visit, live completion, duplicate terminal events, StrictMode, loading, session isolation/revisit, Tab/edit/no-send, Escape, drafts, attachments, pasted blocks, IME, modified Tab and aborted turns.
- The light desktop and dark narrow-screen captures passed the independent review's two corrections: readable theme-aware ghost text and complete wrapped preview above the footer. Captures are under `build/follow-up-prompt-ui/`.
- Strict OpenSpec validation and `git diff --check` passed. Existing unrelated working-tree changes were preserved.
- Layer inspection including new untracked sources found no violations in this change. Two existing R12 file-size findings remain in `desktop_pet_mac.mm` (1006/1000 lines) and `session_registry.cpp` (2418/2400); these files were not edited by this change. The tracked-only CLI also reports unresolved new headers until they are added to Git; the complete-file inspection resolves those headers.
- Reconfigured after the frontend production build and built Windows Release `acecode` and `acecode-desktop`, embedding the updated UI. Existing desktop processes were not restarted.
- Generation transport was verified with a local model fixture; live paid-model quality and macOS/Linux native runs were not performed.

## 4. Draft gate correction

- [x] 4.1 Gate generation on successfully loaded empty drafts, consume skipped completions without requesting and verify controller regressions.
- [x] 4.2 Verify saved-draft restoration races, live unsent text, structured content, clearing/revisiting and empty-draft eligibility in the actual ChatView/browser harness.
- [x] 4.3 Run frontend tests/build and strict OpenSpec checks, refresh the Windows embedded UI and record the results.

Draft-gate correction verification: complete `pnpm test`, `pnpm build`, strict OpenSpec validation and scoped `git diff --check` passed. The browser harness verified zero recommendation requests for restored text, persisted pasted blocks, current unsent text/attachments/pastes at completion, pending draft loads and failed draft loads; clearing/revisiting did not backfill skipped requests, and a confirmed empty draft triggered once. Existing draft-effect unit fixtures were updated for the added load-failure state setter.

The running Windows instance locked the normal `Release/acecode.exe`. The updated daemon and embedded UI were linked into `build/follow-up-draft-preview/acecode.exe` and passed `--version`. That directory reuses the already verified, unchanged desktop shell from this worktree (SHA-256 copy verified), with its helper executables and channel files. A fresh shell link in the shared build tree encountered mixed MSVC-runtime symbols in the existing desktop-support library; this frontend-only correction requires no shell changes. The running instance was preserved.
