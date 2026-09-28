<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The worktree contains the previous default-off Deepin variant and fractional scaling fix. The user reported unexpected exits both around directory selection and while idle. Those launches had a zero core-file limit, so no dump was available. Subsequent sanitizer and minimal Xlib probes reproduced the initialization-order crash described below.

The Linux folder picker currently forks zenity/kdialog; neither exists on this installation. `SplashScreen` is a no-op on Linux, and the Linux host ignores the startup window mode. The sidebar uses `scrollbar-gutter: stable`, which requires a fallback on the installed WebKitGTK 2.38 engine.

## Decisions

1. Initialize Xlib threading at process entry in the dedicated Deepin build,
   before GTK or any other library opens a display. Qt's late `XInitThreads`
   call otherwise leaves existing Xlib resource-database locks null on this
   UOS runtime. The DTK adapter requires successful early initialization and
   retains GTK ownership of the main window/event loop. DTK remains limited
   to rounded corners and shadows. Generic builds do not execute this path.
2. Reuse the Desktop's GTK runtime for native folder selection. Keep GTK dependencies out of headless support targets and distinguish successful selection, cancellation and failure. Existing external picker fallback may remain for contexts without an initialized GTK Desktop.
3. Add a CSS feature-detection fallback that permanently reserves the existing scrollbar width on the session list when `scrollbar-gutter` is unsupported. Preserve the current transparent track and thumb behavior, and verify content width before/after overflow in the actual old WebKit engine.
4. Implement the Linux splash using the existing logo asset and GTK. Keep the main window visually hidden until the frontend is ready, then center and fit it in the active monitor's work area. Reuse existing pure window geometry helpers; preserve user placement on subsequent hide/show operations.
5. Keep the dedicated macro and system shared DTK libraries. Add a package-content gate so no Qt/DTK runtime is accidentally bundled; generic Linux remains toolkit-independent.
6. Stage only the authorized source, tests, docs, workflow and OpenSpec files. Exclude diagnostics, dumps, local configuration and runtime data. Inspect the remote branch before committing/pushing and verify the resulting remote commit.

## Verification

### Reproduced crash and initialization fix

- The UOS user journal records Desktop segmentation faults at 01:47:15 and
  01:51:27. Core generation was disabled for those launches.
- An independent AddressSanitizer build of the actual Desktop reproduced a
  null-pointer crash in `pthread_mutex_lock` called by `XrmQGetResource` /
  `XGetDefault` during GTK text drawing after DTK/Qt initialization.
- A minimal non-sanitized Xlib program reproduces the same failure: open a
  display, call `XGetDefault`, call `XInitThreads`, then call `XGetDefault`
  again. It exits with SIGSEGV (-11). Moving `XInitThreads` before the first
  Xlib call makes the identical resource access succeed (exit 0).
- Preloading early Xlib initialization also allows the previously failing
  sanitized Desktop to start without an ASan report. Production initialization
  now runs before the splash/GTK and guards DTK activation on success.
- Rebuilt the actual fixed Desktop with AddressSanitizer and launched it on
  real UOS X11 without a preload workaround. The frontend became ready and
  DTK advertised `_DEEPIN_NO_TITLEBAR=1`. Approximately six minutes of idle
  observation, twelve native maximize/restore transitions, two folder-picker
  open/cancel cycles and normal shutdown completed with no ASan report;
  process exit status was 0. Full directory selection had already passed in
  the isolated complete Desktop, and the real GTK picker passed selection
  and cancellation independently.
- The final production Desktop compiles with the early initialization fix.
  The actual adapter also recompiles against the pinned DTK 5.2 SDK and the
  linked SDK probe runs with Qt 5.11.3. The existing `loadDXcbPlugin` API emits
  a deprecation warning but remains required by the supported old runtime.
- Xlib's documented contract requires `XInitThreads` to precede every other
  Xlib call: https://xorg.freedesktop.org/archive/X11R7.5/doc/man/man3/XInitThreads.3.html

### Presentation and packaging checks

- System WebKitGTK 2.38 reports no `scrollbar-gutter` support. With the fallback,
  a 300px sidebar retains a 288px row/client width at content heights 20, 800
  and 20px (12px track reserved in all three cases).
- `pnpm test` passed using a temporary Node 22 runtime (the installed Node 20
  cannot import the suite's existing TypeScript files); `pnpm build` passed,
  including the generated-regex compatibility check.
- The actual GTK picker implementation passed cancellation and selection of
  `/tmp` on the real UOS display with X requests synchronized. No zenity or
  kdialog is installed. It also passed on an isolated display with its own
  session bus. UOS replaces GTK file dialogs with a D-Bus dialog service;
  sharing the real session bus with a different test display caused a
  `BadWindow` in `XSetTransientForHint`. This was a test-environment issue,
  not evidence for the user's earlier unexpected exit.
- The complete Desktop bridge was exercised in an isolated X11 session with
  its own D-Bus: open the existing-directory picker, cancel, reopen and select
  the existing ACECode project. The project appeared in the sidebar and home
  screen, and the debugger caught no fatal signal.
- Both updated Desktop variants compiled. The generic executable has no
  Qt/DTK dependencies; the dedicated executable resolves WebKitGTK 4.0 and
  DTK 5, with maximum GLIBC symbol 2.28. A local archive containing both
  executables, logo, model catalog and 111 seed files passed the no-bundling
  gate before compression and after extraction. Actionlint and 10 Python
  package/release verification tests passed. The 16 focused native DPI and
  update tests also passed. ARM and full GitHub matrix execution remain
  unavailable locally.
- Captured the actual startup logo and main window on a 1440x1000 test display
  at native DPI 120. The first 1280x820 main window appeared at (80,90).
  Pointer drag moved it to (115,115); edge resize produced 1180x760. After
  minimize (`_NET_WM_STATE_HIDDEN`) and activation, (115,115,1180,760) was
  preserved. The isolated window manager has no DTK frame-effect support.
- The final Release build was launched through `scripts/dev_desktop.sh` on
  real UOS. Its frontend loaded successfully at page zoom 1.25. Initial
  geometry was (320,105,1280,820), centered in the 1920x1030 work area. The
  Deepin compositor accepted move/resize to (120,120,1000,720) and restoration
  to the initial geometry. This verifies native frame operations; physical
  mouse gestures remain unverified while the desktop is locked. The test
  Release process then shut down normally with exit status 0.
- The 10 existing window-geometry unit tests passed. All three related
  OpenSpec changes pass strict validation, and the complete diff passes
  `git diff --check`.
- Closing the test Desktop through its web close button returned from main
  normally and reached `exit_group` without a fatal signal. The isolated
  test session and helpers were then cleaned up.
- The original user-launched process also remained alive under the debugger
  without a fatal signal. After obtaining the reproducible failure and
  verifying the fix in an independent process, the watcher was detached;
  the user's original window was left running. It must be restarted to load
  the new binary. Actual physical mouse interaction on the locked real
  desktop remains outside the automated verification.
- The remote master advanced from `3992dd62` to `86480019` with a Windows-only
  top-border fix. Fast-forwarded the local branch and reapplied the staged
  Deepin changes cleanly; the upstream Windows code is preserved. Diagnostics
  and local runtime data are excluded from the staged files.
- Rebuilt both executables successfully after integrating upstream. The
  build directory remains configured with `ACECODE_DEEPIN=ON` and WebKitGTK
  API 4.0. All 21 script-verifier tests passed, as did actionlint after the
  explicit X11 development dependency was added.
- Refreshed the local archive with the final fixed binaries and documentation.
  Its 111 seed files and model catalog exactly match the source; the archive
  passes the no-bundling gate before compression and after extraction. Both
  binaries retain the GLIBC 2.28 ceiling and the daemon has no Qt/DTK linkage.
- Implementation commit `ae98590a4245a752204771e976baa7dac71080f2` was
  pushed to `origin/master`, and `git ls-remote` confirmed the same revision.
  GitHub started the push-triggered `test` workflow. Added the Deepin package
  verifier's regression tests to that workflow as well. The dedicated package
  matrix still runs through the repository's manual/tag packaging triggers;
  no release was published during this work.

- Capture and explain the crash; exercise the failing path and idle runtime after the fix.
- Exercise folder picker success and cancel with real GTK, including this machine without external picker programs.
- Measure sidebar width at short/overflowing/short content lengths in system WebKitGTK; run the web suite and regenerate frontend assets.
- Capture the Linux startup logo and verify centered first-show geometry, DPI changes and subsequent window controls.
- Build Deepin and generic Desktop variants, inspect dynamic dependencies, verify package contents and workflow syntax, and run focused native regressions.
- Check the complete staged diff, push to GitHub, and verify the remote revision. Report unavailable ARM/Wayland execution separately.
