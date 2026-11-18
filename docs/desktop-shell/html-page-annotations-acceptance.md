# HTML page annotation acceptance

Specification: [#193](https://github.com/tmoonlight/acecode/issues/193). Implementation tickets: #194–201. OpenSpec: `add-html-page-annotations`. Integration baseline: `0155a863`.

This is the execution checklist, not a claim that the feature is complete. Keep the pull request draft until both platform workflows and required automated checks have passed. Reference-app screenshots and source inspection do not qualify as ACECode acceptance.

## Automated checks

| Check | Result |
| --- | --- |
| OpenSpec strict validation | Passed for initial planning artifacts, 2026-11-18 |
| Annotation interaction through conversation submission | Pending implementation |
| All property previews, text-node identity and saved/draft layering | Pending implementation |
| Batch freeze, queue, receipt, receiver deduplication and failures | Pending implementation |
| Navigation, removed target, document ownership and untrusted commands | Pending implementation |
| Source-selection annotation regressions | Pending |
| Full frontend test suite and production build | Passed at 02920365 on Windows, 2026-11-18; rerun required after remaining tickets integrate |
| Windows native build and focused tests | Native browser smoke target built at 02920365; runtime smoke aborted with 0xc0000409, under diagnosis; full desktop/unit build in progress |
| macOS native build and focused tests | Not executed; macOS environment required |
| Standards and specification code review | Pending |

## Real desktop workflow

Run the following independently on Windows and macOS against the integrated build. Record build commit, OS, commands, observations and evidence paths. Use an already-loaded local-service page and a remote page, with nested text elements and a long page containing recognizable content below the viewport. A runtime test or successful native API return is not sufficient proof of screenshot pixels.

The local fixture is `tests/desktop/helpers/html_annotation_fixture.html`. Serve that directory over HTTP and open the page in ACECode Agent Browser, not standalone file preview. Its orange middle and green `FULL-PAGE-CAPTURE-END` section distinguish full-page pixels from a viewport capture. `window.annotationFixture.childIdentityPreserved()` checks the original nested element identity; the counter button checks retained listener behavior. The two identical lifecycle paragraphs test that removing the original never relocates an annotation to its sibling. Record a fixture file hash before and after previewing to check that no source file was written.

| Step | Windows | macOS |
| --- | --- | --- |
| Toolbar enables mode; element click outlines; control opens trusted popup above browser | Not executed | Not executed |
| Text editing preserves nested child elements and their event behavior | Not executed | Not executed |
| Text color, background, opacity, font, font size and weight preview | Not executed | Not executed |
| Radius, border color and width, width, height, spacing, padding and margin preview | Not executed | Not executed |
| Ratio lock starts off and captures current ratio when enabled | Not executed | Not executed |
| Unchanged values produce no differences; cancel preserves previous saved preview on same element | Not executed | Not executed |
| Multiple elements and one replaceable region share description and pin; region has no property editing | Not executed | Not executed |
| Save produces pin/count but no model work and no HTML/CSS file writes | Not executed | Not executed |
| Original comparison keeps pins; restoring previews and leaving mode retain saved work | Not executed | Not executed |
| Explicit batch send yields one message and exactly one full-page image with below-viewport pixels | Not executed | Not executed |
| Busy model receives queued new turn, not steering; composer draft and attachments remain | Not executed | Not executed |
| Session switching cannot retarget frozen batch; additions during send remain pending | Not executed | Not executed |
| Accepted batch clears only its pending count, retains pins/previews, and subsequent numbers increase | Not executed | Not executed |
| Capture/upload failure sends nothing; uncertain receipt retries same identity without duplicate work | Not executed | Not executed |
| Navigation/refresh defaults to staying; replacement/removal invalidates without relocation | Not executed | Not executed |
| No usable conversation permits annotation but blocks sending without creating a conversation | Not executed | Not executed |
| Existing source-selection annotation workflow remains functional | Not executed | Not executed |

## Recording failures

Keep failed and unavailable checks explicit. Do not mark a task complete because a platform implementation exists or because another platform passed. Invalid saved annotation records cannot be repaired by introducing an unapproved edit/delete UI. Capture resource limits must report failure rather than silently truncate. Runtime retention does not imply restart recovery.

## Execution record

- 2026-11-18, Windows, edd093ae: `pnpm install --frozen-lockfile`, `pnpm test` and `pnpm build` completed successfully in the integration worktree. The test log contains 3,672 `[pass]` lines plus the Node test-runner summary. Non-fatal warnings: Node local-storage file setting and existing large frontend chunks. These are baseline results before product implementation, not annotation acceptance.
- 2026-11-18, Windows, 02920365: integrated the single-element annotation loop, explicit full-page capture and receiver-side batch receipts. `pnpm test` passed with 3,677 `[pass]` lines plus five Node test-runner tests; `pnpm build` and regex compatibility checks passed. `agent_browser_host_smoke` compiled and linked using the x64 MSVC desktop preset with the vcpkg `tests` feature. These checks do not establish the remaining annotation features or real desktop acceptance.
- 2026-11-18, Windows, 02920365: strict static checks found new native-host line-budget overruns, a misplaced shared test helper, an ambiguous bridge capture and a lexical ownership false positive for the `<new>` header. Fixes are in progress; documentation-path validation passed. Do not treat the combined command's final exit status as proof that all earlier checks passed.
- 2026-11-18, Windows, 02920365: executing `agent_browser_host_smoke.exe` aborted with exit status `0xc0000409` before emitting a smoke result. Windows Application events 1000/1001 report `abort` in the executable. Native capture pixels and interaction acceptance remain unverified; a temporary debugger harness outside the repository is collecting the failure stack.
- 2026-11-19, Windows, be66b007: an isolated MSVC/GoogleTest executable passed all 20 focused tests: six annotation-session tests, eleven full-page capture contract tests and three annotation-submission request tests. This does not replace native capture or complete desktop acceptance.
- 2026-11-19, Windows, be66b007: the native smoke abort reproduced without a debugger. A lightweight temporary debugger identified an uncaught `nlohmann::json::type_error` in the smoke capture probe: the full-page command returned a non-object, and the probe called `.value("data", "")` before checking the native error. The test helper must preserve and report that capture failure instead of terminating its worker thread. Slower debugger runs also encountered the external example-page navigation prerequisite timeout; neither outcome establishes native capture success. Full native compilation remains in progress.
