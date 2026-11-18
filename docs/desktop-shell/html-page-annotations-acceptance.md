# HTML page annotation acceptance

Specification: [#193](https://github.com/tmoonlight/acecode/issues/193). Implementation tickets: #194–201. OpenSpec: `add-html-page-annotations`. Integration baseline: `0155a863`.

This is the execution checklist, not a claim that the feature is complete. Keep the pull request draft until both platform workflows and required automated checks have passed. Reference-app screenshots and source inspection do not qualify as ACECode acceptance.

## Automated checks

| Check | Result |
| --- | --- |
| OpenSpec strict validation | Passed for initial planning artifacts, 2026-11-18 |
| Annotation interaction through conversation submission | #201 controller/runtime/session/API/queue seam exercised at 8f2e060e; standalone browser and injected capture/HTTP boundaries, not a real desktop workflow |
| All property previews, text-node identity and saved/draft layering | Real Chromium style fields and multi-target saved/draft/original/cancel passed; overlapping parent/child text diff regression F2 remains red at 8f2e060e |
| Batch freeze, queue, receipt, receiver deduplication and failures | #201 frozen records/session/API, mismatched receipt retry and busy ordinary dedup passed at 8f2e060e; real HTTP-refusal/target-clone retry reproduces F1; final rerun required |
| Navigation, removed target, document ownership and untrusted commands | Exact clone, page/document/membership and stale-callback rejection passed at 8f2e060e; native navigation and #200 integration remain unverified |
| Source-selection annotation regressions | Own frontend suite/source API payload seam passed; coordinator's 8f2e060e native run includes seven source-selection tests; final rerun required |
| Full frontend test suite and production build | #201 own worktree based on 8f2e060e: pnpm test and pnpm build passed; rerun required after remaining tickets integrate |
| Windows native build and focused tests | Coordinator reports 8f2e060e unit/desktop/smoke targets built and 58 focused tests passed; actual native smoke failed capture_document_changed without abort; not native capture acceptance |
| macOS native build and focused tests | Not executed; macOS environment required |
| Standards and specification code review | Pending |

## High-seam automated regression (#201)

The opt-in `pnpm test:annotations:integration` suite reuses the existing real Chromium
browser fixture and isolated-world runtime. Unlike sender unit fixtures, its saved
records and differences originate from trusted mouse input, the production frontend
controller, `PageAnnotationSession::prepare/complete`, and the injected production
text/style/target runtimes. It exercises:

- Multiple element targets plus one region, actual text/style differences, original
  comparison, saved/draft cancel, nested node identity and retained listeners.
- Saved records flowing into the production batch sender and API client, frozen
  membership/session/credentials/page metadata, additions during capture, one PNG
  upload, a mismatched receipt, stable retry and native acknowledgement.
- Production identity parsing and busy `AgentTaskQueue` admission/deduplication;
  ordinary source-selection/composer payload serialization with existing attachments.
  Existing composer callback and source-selection suites remain in `pnpm test`.
- Document/page/record ownership, untrusted command fields, exact-target clone
  replacement, invalidated retained records and late native callback rejection.
- Explicit HTTP refusal followed by real DOM target replacement: retry must stop
  before HTTP admission, retain pending work and leave the busy queue unchanged.
- Overlapping parent/child text differences checked against the final real DOM,
  rather than against requested preview values.

Build the test-only native driver outside the repository, using an existing
read-only dependency include prefix. No CMake configure, shared build graph,
vcpkg installation, native host or daemon is started by this command:

```powershell
tests/test_support/desktop/build_annotation_integration_driver.ps1 `
  -DependencyInclude C:/path/to/installed/triplet/include `
  -OutputDirectory "$env:TEMP/acecode-annotation-integration"
$env:ACECODE_TEST_ANNOTATION_SESSION = "$env:TEMP/acecode-annotation-integration/annotation_integration_driver.exe"
$env:ACECODE_TEST_BROWSER = 'C:/path/to/msedge.exe'
cd web
pnpm test:annotations:integration
```

The driver links the production queue and append-file implementation, not stubs.
The build helper initializes x64 MSVC when needed; other installations can supply
`-VcVarsPath` or use an initialized developer shell. The dedicated integration
command fails when the driver is missing; `test:annotations:browser` can still run
the pre-existing CSS tests independently and explicitly reports the native seam
as skipped when no driver is configured.

Boundary limitations: capture transport is injected using a standalone Chromium
PNG and an independent opaque capture token. The loopback HTTP adapter invokes the
real identity parser/queue, not the daemon routes, attachment storage, persisted
session manager or model. Existing web-server smoke and receiver tests own those
boundaries. This suite does **not** establish WebView2/WKWebView full-page pixels,
native navigation prompts, complete ChatView interaction, platform compilation,
manual acceptance, or restart recovery. A PNG-height assertion is not native
below-viewport pixel acceptance.

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

- 2026-11-19, coordinator-provided integration evidence at `6f6b491c`: full `pnpm test` exited 0 after the three ownership-path assertion fixes were integrated; TEMP `acecode-annotations-pathfix-web-test.log`. The coordinator reports this head pushed to draft #202 with a new CI run in progress; no result for that run is claimed here. Product review-fixes and #200/#201 integration still require fresh frontend/native verification. The earlier `8f2e060e` macOS installer CI pass does not establish macOS native build or runtime acceptance.
- 2026-11-19, #201 additional high-seam red test at `1c627535` plus test-only work: a real API HTTP `503 annotation_batch_unavailable`, followed by replacing the annotated node with an identical clone, reproduces review finding F1. Native validation independently rejects the clone, but the old sender retry returns success and first-admits the invalid batch. TEMP `acecode-201-browser-f1.log`. This latest run stops at F1; the earlier separate run reached and reproduced F2 below. Neither failure is waived or changed to a skip, and both fixes remain assigned to the product review-fixer.
- 2026-11-19, coordinator checkpoint: the full native suite is still running and has already reported a `PointerOverlay` failure involving a `Shell_SystemDim` hit. No full-suite pass or reproduced baseline failure is claimed. Final failure inventory and logs remain to be recorded after completion. The focused 58-test result below is not a substitute for this broader run.
- 2026-11-19, coordinator-provided CMake harness evidence at `8f2e060e` (not independently rerun by #201): `ctest --test-dir build/windows-x64-desktop-release --output-on-failure -j4 -R '(PageAnnotation|AgentBrowserCapture|AgentBrowserNativeBoundaries|AnnotationSubmission|SelectionContextAnnotation|WebServerHttp.AnnotationBatch)'` passed all 58 tests in 9.08 seconds; TEMP `acecode-annotations-integrated-ctest.log`. This confirms discovery/execution through the integrated CMake harness, not native screenshot pixels or Windows/macOS manual acceptance. Rerun after all worker changes merge.
- 2026-11-19, coordinator-provided evidence at `8f2e060e` (not independently rerun by #201): MSVC incremental targets `acecode_unit_tests`, `acecode-desktop`, `agent_browser_host_smoke` exited 0; TEMP `acecode-annotations-native-integrated-build.log`. Filter `*Annotation*:*AgentBrowserCapture*:*AgentBrowserNativeBoundaries*` passed 58 tests in eight suites (1705 ms), including two actual `WebServerHttp` annotation receiver tests and seven source-selection tests; TEMP `acecode-annotations-integrated-native-tests.log`. Running the native smoke took 3.8 seconds and explicitly **failed** with `capture_document_changed`, without the former abort; TEMP `acecode-annotations-integrated-native-smoke.log`. This supersedes neither complete Windows manual acceptance nor the required all-worker integrated rerun. Coordinator also reports strict layer/file-size/ownership-final/doc-path/OpenSpec checks and all 104 refactor Python tests passed at this base; those reports are baseline evidence, not #200/#201 acceptance.
- 2026-11-19, #201 worker based on `8f2e060e`: own-worktree `pnpm test` passed (including the native extraction ownership-path fixes, source-selection and composer callback regressions); `pnpm build` and regex compatibility passed. Non-fatal Node local-storage and large-chunk warnings remain. Test-only native integration driver compiled with x64 MSVC 14.44, using the existing `x64-windows-static/include` dependency prefix read-only and TEMP outputs; no shared native build was used. `pnpm test:annotations:integration` passed the existing real CSS browser cases and the new interaction/save, frozen batch/API/queue/receipt, and ownership/stale-callback seams, then **failed** its overlapping text assertion: child diff `Earth` versus actual DOM `world` (review finding F2, separately assigned for product correction). Evidence: TEMP `acecode-201-{web-test,web-build,native-driver-build,browser}.log`. This is a recorded regression, not a passing integration result; #200 and the product seam fixes require a fresh integrated rerun. No platform manual row is changed.
- 2026-11-18, Windows, edd093ae: `pnpm install --frozen-lockfile`, `pnpm test` and `pnpm build` completed successfully in the integration worktree. The test log contains 3,672 `[pass]` lines plus the Node test-runner summary. Non-fatal warnings: Node local-storage file setting and existing large frontend chunks. These are baseline results before product implementation, not annotation acceptance.
- 2026-11-18, Windows, 02920365: integrated the single-element annotation loop, explicit full-page capture and receiver-side batch receipts. `pnpm test` passed with 3,677 `[pass]` lines plus five Node test-runner tests; `pnpm build` and regex compatibility checks passed. `agent_browser_host_smoke` compiled and linked using the x64 MSVC desktop preset with the vcpkg `tests` feature. These checks do not establish the remaining annotation features or real desktop acceptance.
- 2026-11-18, Windows, 02920365: strict static checks found new native-host line-budget overruns, a misplaced shared test helper, an ambiguous bridge capture and a lexical ownership false positive for the `<new>` header. Fixes are in progress; documentation-path validation passed. Do not treat the combined command's final exit status as proof that all earlier checks passed.
- 2026-11-18, Windows, 02920365: executing `agent_browser_host_smoke.exe` aborted with exit status `0xc0000409` before emitting a smoke result. Windows Application events 1000/1001 report `abort` in the executable. Native capture pixels and interaction acceptance remain unverified; a temporary debugger harness outside the repository is collecting the failure stack.
- 2026-11-19, Windows, be66b007: an isolated MSVC/GoogleTest executable passed all 20 focused tests: six annotation-session tests, eleven full-page capture contract tests and three annotation-submission request tests. This does not replace native capture or complete desktop acceptance.
- 2026-11-19, Windows, be66b007: the native smoke abort reproduced without a debugger. A lightweight temporary debugger identified an uncaught `nlohmann::json::type_error` in the smoke capture probe: the full-page command returned a non-object, and the probe called `.value("data", "")` before checking the native error. The test helper must preserve and report that capture failure instead of terminating its worker thread. Slower debugger runs also encountered the external example-page navigation prerequisite timeout; neither outcome establishes native capture success. Full native compilation remains in progress.
