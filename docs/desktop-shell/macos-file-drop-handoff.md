# macOS file-drop regression handoff

> 历史状态：本文是修复实施前的交接快照，其中“尚未实现”的描述不再代表当前状态。
> 问题已经修复并归档；当前设计、平台影响、日志策略和回归清单见
> [`macos-file-drop.md`](macos-file-drop.md)。本文只保留用于追溯当时证据与决策过程。

## User request and current status

Continue fixing Finder file drops into the ACECode composer without regressing
typing. The user requested this handoff to resume in a new conversation.

An earlier experimental fix broke typing. It has been rolled back to baseline
behavior, with diagnostic logging retained. The rollback diagnostic build has
been built and launched by the user. New logs locate the remaining rejection
in the frontend hover gate. The proposed coordinate-based fix below has NOT
been implemented or tested; the latest request is documentation only.

## Environment

- Repository: `/Users/hudy/Desktop/GitHubCode/acecode`
- Branch: `feat/macos-custom-installer-updates`
- HEAD: `288133c7`
- Current machine: Intel x86_64, macOS 15.7.9, SDK 15.2 / AppleClang 16.
- The M5/macOS 26 machine mentioned earlier is a DIFFERENT computer.
- Node: `$HOME/.nvm/versions/node/v22.22.2/bin` (use Node 22, not Node 20).
- pnpm: `corepack pnpm@10.11.0`.
- Python for packaging tests: `/usr/local/bin/python3.10`.
- vcpkg: `$HOME/vcpkg`, dependencies installed.
- Proxy if needed: `export all_proxy=http://127.0.0.1:7890`.
- Build directory: `build/macos-x64-release`.
- Existing isolated build tools and logs:
  `/Users/hudy/Desktop/GitHubCode/acecode/.acecode/tmp/session-20260918-145721-6662`.
  Its `build-tools/bin` contains working CMake 3.31.10 and Ninja. A new
  session's `$ACECODE_TMPDIR` will point elsewhere; do not assume tools migrated.
- Homebrew on this machine is old and was incompatible with the OS. Avoid
  relying on it or the broken default CMake wrapper.

Do not stop, replace, or launch another running ACECode instance automatically.
This conversation itself runs in ACECode. Ask the user to fully quit before GUI
testing a new native binary. Closing the window may leave a tray instance.

## Evidence from the user's latest test

These are the relevant new log messages, excluding historical failed builds:

```text
01:13:11.594 [file-drop] macOS installation class=NSKVONotifying_WKWebView handler=available
01:13:14.941 [file-drop] composer-focus {"disabled":false}
01:13:16.726 [file-drop] composer-input {"disabled":false}
01:17:10.867 [file-drop] macOS draggingEntered file_urls=true class=NSKVONotifying_WKWebView handler=available
01:17:11.940 [file-drop] macOS performDragOperation file_urls=true class=NSKVONotifying_WKWebView handler=available
01:17:11.941 [file-drop] macOS performDragOperation count=1
01:17:11.942 [file-drop] bridge count=1 consoleReceiver=true composerReceiver=true
01:17:11.942 [file-drop] composer-rejected {"disabled":false,"hover":false,"ageMs":-1,"count":1}
```

Confirmed:

- The rollback native implementation is running.
- The composer received at least one input event. This is not proof of all
  English/Chinese/IME/paste scenarios working; the user's manual check remains
  authoritative.
- AppKit receives the file drag and drop and extracts one file path.
- The JS bridge finds both legacy receivers.
- The composer rejects before materialization because its DOM hover state is
  inactive and has no timestamp. File reading/materialization has not failed;
  it was never attempted for this drop.

Not yet proven:

- Whether DOM drag events never arrive or the hover state clears before the
  native callback. The native file branch returns copy without forwarding to
  WebKit's original draggingEntered, which is a hypothesis worth inspecting.
- Whether the earlier runtime subclass operation was the sole cause of the
  typing regression. It is a strong suspect, not a demonstrated root cause.
- Whether green copy cursor and hover styling now work; logs alone cannot say.

Full local logs previously examined:
`~/.acecode/logs/desktop-2026-09-19.log` and relevant daemon errors.
The broken startup reached daemon_connected, dom_ready and ui_ready, without
corresponding JS errors in the examined 00:45:45-00:47:55 interval.

Important startup message:

```text
00:45:48.594 [desktop] dev mode: serving web/ from /Users/hudy/Desktop/GitHubCode/acecode/web/dist (file changes hot-reload on F5)
```

The desktop may use filesystem `web/dist` rather than embedded assets. An old
binary can therefore load a new frontend. Check startup asset-source logs when
comparing versions. Rebuilding dist can affect a running development instance
on reload; do not equate packaging isolation with frontend isolation.

## Current implementation / retained changes

- `src/apps/desktop/web_host.cpp`: baseline class-wide swizzle and legacy callbacks
  restored. Only native installation/enter/drop/count logs remain relative to
  HEAD. Entry logging now runs before the file-type branch.
- `src/apps/desktop/web_host.hpp`: restored to HEAD.
- Removed the failed object_setClass runtime subclass, instance mapping,
  NativeFileDragHandler API, lifecycle bridge and capability flag.
- `src/apps/desktop/main.cpp`: legacy dual console/composer callbacks restored;
  added count and receiver-availability diagnostics.
- `web/src/components/InputBar.jsx`: original native hover gate restored;
  logs rejection/materialization and passive focus/input event presence.
- `web/src/lib/macNativeFileDrag.js`: currently a diagnostics-only helper,
  not a router. No text, keystrokes, file paths or exception payloads logged.
- `web/src/components/ConsoleDock.jsx` and `web/src/lib/runTests.js`: baseline.
- Earlier experimental macNativeFileDrag.test.js was removed with the rollback.
- `scripts/macos_create_portable_zip.sh`: retain the independent lipo fix:
  `lipo "$binary" -verify_arch "$arch"`. Its script test mock was updated.
- `docs/desktop-shell/macos-file-drop-debug.md`: local build/test instructions.
- Existing ignored OpenSpec change:
  `openspec/changes/fix-macos-native-file-drop/`. Design records rollback;
  tasks mark rollback/build complete, manual typing verification still open.
  Earlier checked implementation tasks are historical, not the current design.

## Proposed next implementation (not yet done)

There is enough evidence to design a targeted fix; no standalone logging-only
iteration is required first. Inspect the current source and repository rules
before implementation, then update the existing OpenSpec change.

1. Keep the restored native mechanism. Do NOT restore object_setClass or the
   failed runtime subclass design.
2. Attach the native drop location to the drop callback and bridge payload.
   Carefully convert AppKit coordinates to CSS viewport coordinates; verify
   flipped coordinate systems, WebView bounds and zoom rather than assuming
   Retina pixels equal CSS pixels.
3. Hit-test the actual frontend drop target. Deliver to the composer only when
   the point is inside its eligible, unoccluded area; deliver to terminal only
   when appropriate. Reject other targets, disabled inputs and modal overlays.
4. For valid coordinate-bearing native drops, stop depending on DOM hover state.
   Do not simply remove the hover check globally or blindly send files to both
   receivers. Preserve legacy/browser/other-platform behavior deliberately.
5. Add small diagnostic messages for coordinate validity, selected target and
   rejection/result, without private paths or input content.
6. Add focused pure-helper tests and run full frontend tests and build and native
   build. Test duplicate delivery, stale/no hover, disabled/covered composer,
   wrong target, coordinate boundaries and backward compatibility.
7. Verify typing first on the real app, then local Finder drops. Drop success
   and drag-hover feedback are separate requirements: a drop-coordinate fix
   alone does NOT guarantee hover styling or copy cursor feedback. Investigate
   that separately without reintroducing invasive native changes.

The prior assistant recommended this approach; it is not yet a verified fix.

## Build and verification already completed

- Node 22 full frontend tests: 2552 pass lines, exit success.
- Frontend production build and regex compatibility check passed.
- Native desktop/daemon build and portable archive verification passed.
- Packaging Python tests: 4/4 passed.
- `git diff --check` passed at rollback completion.
- Packaged CLI reports `acecode v0.9.20`.
- No automated AppKit/Finder or IME verification was performed.

Build commands used (run from repository root unless otherwise specified):

```sh
export PATH="$HOME/.nvm/versions/node/v22.22.2/bin:/usr/local/bin:$PATH"
(cd web && corepack pnpm@10.11.0 test && corepack pnpm@10.11.0 build)

export all_proxy=http://127.0.0.1:7890
export PATH="/Users/hudy/Desktop/GitHubCode/acecode/.acecode/tmp/session-20260918-145721-6662/build-tools/bin:$PATH"
export VCPKG_FORCE_SYSTEM_BINARIES=1 CMAKE_BUILD_PARALLEL_LEVEL=4
export VCPKG_ROOT="$HOME/vcpkg"
bash scripts/macos_create_portable_zip.sh rollback-diagnostics --arch x64

/usr/local/bin/python3.10 tests/scripts/macos_portable_package_test.py
git diff --check
```

The packaging script refreshes configure-time embedded web assets; merely
running cmake --build after changing web/dist is insufficient.

Current outputs:

- `dist/acecode-macos-x86_64/ACECode.app`
- `dist/acecode-0.9.20-macos-x86_64-rollback-diagnostics-portable.zip` (~42 MB)
- ZIP SHA-256:
  `a4eee52eb15941330385ede6e0cf3785e7edf32e731a5a963d11119b02fb7ff3`

Packaging DID update the App at that dist path. It did not intentionally
replace `/Applications/ACECode.app` or restart the running process. A new
suffix protects the old ZIP, but does NOT give the staging App a unique path.
Warn about this distinction before rebuilding if it is the user's active App.

Manual launch, only after fully quitting the old instance:

```sh
open "/Users/hudy/Desktop/GitHubCode/acecode/dist/acecode-macos-x86_64/ACECode.app"
```

Collect diagnostic logs and note the actual test time:

```sh
grep -h '\[file-drop\]' "$HOME"/.acecode/logs/desktop* | tail -200
```

Historical logs from the failed build contain `ACEFileDrag_...` and
`target-registered`. Do not confuse them with new test results.

## Working tree safety / conventions

No commit or push was made for this work. Preserve unrelated changes.
At handoff, git status additionally shows modifications to `.gitignore` and
`AGENTS.md`, and an untracked `.comet/` directory. These were not part of the
rollback changes listed above; investigate if needed and do not revert them.

Read current AGENTS.md/AGENTS.md and follow the active session's instructions.
Use apply_patch for manual edits, and do not change vendored/submodule sources.
Codegraph was unavailable/not indexed in the previous session; ordinary bounded
source inspection was used. `rg` is not installed; use grep or built-in tools.
Do not kill ACECode, overwrite installed apps, commit or push without user intent.
