# Design

## Context

See proposal.md for motivation. Pet hosts share 344×252 compact scene geometry and the desktop-office snapshot/action bridge. Mini temporarily expands its transparent native surface to the monitor work area for activity bubbles. Animated pets share one page; mini and the office have dedicated pages. The supplied images and subsequent user corrections are the visual authority for mini.

## Goals / Non-Goals

Keep mini within the existing desktop-pet lifecycle, preferences and settings. No new daemon endpoint, sprite assets, recording backend or global shortcut is required. Other in-progress worktree changes stay intact.

## Decisions

- Share the conversation send-control definition in [composerActionAppearance.js](../../../web/src/lib/composerActionAppearance.js) with [ComposerActionButton.jsx](../../../web/src/components/ComposerActionButton.jsx). Measure the resolved main-renderer control, including its theme and disabled appearance, and convert between main and pet device-pixel ratios. The canonical default is 28 logical pixels with a 14px paper-plane icon; navigation is three control widths by one control height, and recording is two by one.
- Mini has a default native logical scale of 1 and persists its own zoom in `mini-settings.json`. Legacy character-pet zoom in `settings.json` does not affect mini. Explicit mini zoom still scales its compact controls.
- During activity expansion the transparent native mini surface temporarily spans its monitor work area. The normal window rectangle is retained as the toolbar anchor, and only reported visible controls/bubbles receive input. The page lays out natural-height bubbles below the anchor, flips upward when needed and clamps to work-area bounds. Closing, dragging, resizing, screen changes and switching pet restore the compact surface. Overflow remains wheel/keyboard accessible with scrollbar chrome hidden.
- Use a dedicated embedded mini page without animation sheets. Mini and the classic pets load the same [PetActivityCards renderer and styles](../../../assets/desktop_pet/pet_cards.js), including transcript font, status icons, colors, shadows and open/stop/read actions. Preserve the white navigation/input capsule; activity bubbles follow the shared light/dark palette.
- Reuse structured `activities` for titles, status and session identities. Count running and waiting/unread tasks, including accepted mini submissions while the next snapshot catches up. Never infer state from display text. No greeting or persistent helper copy.
- Mini owns its composer and independent recording state. Text and voice submissions create and queue background tasks through existing authenticated APIs, without main-editor navigation or main-window activation. An App-owned controller publishes acknowledgements and recording state in the office snapshot. The native host grants keyboard focus only to mini while composing; explicitly opening a session card retains the existing session-navigation action.
- Report capsule/card shadow bounds through the existing overlay protocol. Start native drag after 5px of movement measured in client coordinates, suppress the following click, and retain the native menu and resize protocol. Mini never docks into peek mode.

## Built surface

Mode: **Operate**. The implementation in [mini_pet.html](../../../assets/desktop_pet/mini_pet.html) follows the supplied Codex mini navigation screenshots.

- The transparent canvas contains a white (`#fff`) navigation/input capsule. Compose, waveform and bell icons use dark ink (`#202123`), with fine dividers (`#eeeeef`) and a pale hover fill (`#f5f5f5`). The 14px blue badge (`#0873d4`) has white text at 4.76:1 contrast. Capsule/input shadows are `0 7px 24px rgba(0, 0, 0, .16), 0 2px 7px rgba(0, 0, 0, .04)` at base scale; offsets and blur scale with the compact geometry.
- With the default 28px measured control, equal device-pixel ratios and mini scale 1, navigation is 84×28px and recording is 56×28px. Recording retains compose and a dark stop dot. These sizes derive from the shared control instead of a separately estimated capsule height.
- Compose opens a one-line input pill up to 330px wide at base scale. Its attachment/send controls use the measured control size with 8px vertical padding: 44px total height for the default 28px control, above the 40px minimum. Send uses the same paper-plane SVG, resolved theme colors, disabled colors/opacity, radius and shadow as the main conversation button. Attachment chips wrap and scroll at small window sizes; failed voice sends have a separate transcript preview with retry/cancel actions.
- The collapsed bell counts running and waiting/unread tasks up to `99+`; zero hides the badge. Expanding replaces the bell and badge with a chevron. Accepted mini submissions appear immediately as pending running cards and merge by session identity when observed in the next snapshot.
- Activity bubbles use `PetActivityCards`, also loaded by [classic_pet.html](../../../assets/desktop_pet/classic_pet.html) and [classic_pet.js](../../../assets/desktop_pet/classic_pet.js). They inherit the live transcript font (14px fallback, line-height 1.6), a bold title, status/body line, status icon and running-session stop control. Shared geometry uses a 3.3em minimum height, 1.6em radius and 6px list gaps. Shared colors include light `#fff` / dark `#2b2d31` surfaces with matching text and shadows. Hover/focus exposes dismiss; completed results are marked read, while other cards are temporarily hidden. Open, stop and dismiss retain keyboard behavior. Natural-height bubbles fit below the compact anchor when possible, otherwise above, and clamp to the work area; excess content scrolls without visible scrollbar chrome.
- Buttons retain accessible names and a visible blue focus outline. Escape hides the composer while retaining its draft, cancels an active recording, or collapses the activity list and returns focus from a card to its toggle. Empty and disconnected states occupy the same card surface without persistent helper copy. Session and attachment rows retain stable keys across snapshot updates.
- The [settings preview](../../../web/src/components/VirtualOfficeSettings.jsx) uses the settings theme's background, foreground and border tokens. Its capsule fills available card width up to 118px, and its three 17px icons shrink within the narrow selection card layout. Selection uses the existing radio-card border and dot, with immediate persistence through the existing settings handler.

## Independent input and scaled shadows

The corrected composer and recording screenshots govern these states. [miniTaskController.js](../../../web/src/lib/miniTaskController.js), owned by [useDesktopOffice.js](../../../web/src/lib/useDesktopOffice.js), snapshots workspace context, creates a session without activating it, uploads selected files and queues the first message. Request IDs suppress repeated clicks and reuse an already-created session on retry. Only accepted sends add activity cards; snapshots then replace those temporary cards without duplication. The mini page receives state and receipts, not API credentials.

The text draft and selected attachments remain independent of voice capture, successful voice sends and voice failures. A failed text send retains its draft and files. A failed voice send keeps its transcript in separate recovery state; retry uses that transcript and `files: []`, and cancel discards only the voice recovery. Files cross the native bridge in bounded chunks, with a maximum of 10 files and 25MiB total, then use the existing session-attachment API.

Recording shares the ownership/cancellation state machine in [speechSession.js](../../../web/src/lib/speechSession.js) and the configured speech backend, independently of any editor. [miniSpeechAudio.js](../../../web/src/lib/miniSpeechAudio.js) uses MediaRecorder and offline WAV conversion so capture does not require an autoplay gesture in the hidden main renderer. Stop, including the duration limit, transcribes and sends automatically. Cancel or close during permission, capture or transcription invalidates callbacks and releases tracks, preventing a late transcription from starting a task.

Capsule, composer and error shadows scale with mini geometry; activity bubbles retain the shared renderer's typography and shadows. Reported native paint regions include padded shadow tails for all visible surfaces. The expanded window's unused transparent space stays outside the native input region. Zoom, reset, drag and resize restore the saved compact rectangle before acting, so the full-work-area surface cannot become the new size or anchor. Browser verification checks pixels outside reported regions as well as the visible page.

## Risks / Trade-offs

- Compact input and attachments remain bounded by the saved pet window. Expanded activity bubbles use the monitor work area, with wheel and keyboard access when the list exceeds it.
- Input and capture depend on the shared main renderer remaining alive, while the main window may stay hidden.
- Native smoke tests exercise the real Windows host and bridge with mocked task APIs and a fake microphone. They do not validate a live daemon/model job or physical microphone. macOS is covered by the browser shim only; no native macOS build or run was performed.

## Verification

### Current results — shared controls, bubbles and expanded surface

- Full `pnpm test` and `pnpm build`: passed. Logs: `%TEMP%/ace-mini-shared-web-tests.log` and `%TEMP%/ace-mini-final-web-build.log`.
- Configure/build of `acecode-desktop` and `mini_pet_input_smoke`: passed with current embedded resources. Log: `%TEMP%/ace-mini-final-desktop-build.log`. Targeted native layout/service tests: 35/35 passed, including independent mini default/saved scale behavior.
- `test-mini-desktop-pet.mjs`: passed both host shims with 20 captures; maximum alpha outside reported native regions remained at most 3/255. Captures: `%TEMP%/ace-mini-pet-3Nwdmq/`.
- `test-pet-bubbles.mjs`: passed shared markup/computed-style equality, stop/read keyboard actions, below/above/clamped placement with 20 cards, hidden scrollbars and keyboard access to the last card. Captures: `%TEMP%/ace-shared-pet-bubbles-dqGPiM/`. Full classic-pet browser regression also passed for clippy, rover and whale: `%TEMP%/ace-pet-browser-WHxIeC/`.
- Native `test-mini-background-input.mjs` passed through the actual Windows `WebHost`, `DesktopPet` and bridge. Main and mini send buttons measured the same 28 physical pixels and shared color/SVG path. It verified five accepted fixture tasks, retained drafts, chunked-attachment fidelity and voice recovery while the main HWND remained hidden and non-foreground. Expanded popup checks covered preserved anchor, below/above placement, work-area bounds, native regions including controls/bubbles while excluding central blank space, and zoom restoring compact bounds before resize/reset. Results: `%TEMP%/ace-mini-input-4MdeCi/`.
- The equivalent browser run at device-pixel ratio 1.5 measured both send controls at 42 physical pixels. Results: `%TEMP%/ace-mini-input-0x6MtT/`.
- Strict OpenSpec validation and `git diff --check`: passed. Fresh independent review of the shared-control/bubble UI: **ship**, with no material UI fixes requested; this documentation update records that reviewed implementation.
- The built executable is `build/windows-x64-desktop-release/acecode-desktop.exe`. The user's running `build/windows-x64-msvc` instance was not replaced or restarted. Native smoke uses mocked task APIs and a fake microphone; no live model job, physical microphone or native macOS build/run was validated.

### Historical initial implementation results — 2026-10-31

These historical results cover the initial shortcut-based implementation before the user's correction to independent background input. Its main-composer flow and review verdict are superseded by the follow-up below.

- `pnpm test` and `pnpm build`: passed. The new unavailable-composer message is included in the generated translation catalog.
- `node web/scripts/test-mini-desktop-pet.mjs`: passed with both Windows bridge and macOS shim, 16 captures; covers notification updates, focus retention, session navigation, scrolling, collapse hit regions, drag/click separation and drag with zero screen coordinates.
- `node web/scripts/test-virtual-office-preferences.mjs`: passed, including actual `SettingsPage` selection, close/reopen persistence and light/dark/390px layouts.
- `node web/scripts/test-speech-recognition.mjs`: passed with browser test audio; mini respects settings overlays, reports unconfigured speech and enters the existing recording/cancellation flow.
- MSVC build of the current desktop-pet layout and office-service tests: 33/33 passed.
- Final `cmake --build build/windows-x64-desktop-release --target acecode-desktop -j 4`: passed after refreshing embedded frontend assets. An initial link failure was resolved by recompiling stale `im_channels.cpp.obj`, which referenced the previous side-chat signature; no side-chat source changes were needed.
- `openspec validate add-mini-desktop-pet --strict` and `git diff --check`: passed. Independent visual review: ship; both drag compatibility and badge contrast findings resolved.
- Native Windows desktop interaction was not manually exercised. macOS was checked through the browser shim only, without a native macOS build or run.

### Historical follow-up results — independent background input

These results precede the shared-control/bubble and expanded-surface corrections. The current results above supersede their UI and build claims.

- Mini browser tests passed for both host bridges, including background submissions, automatic voice send, cancellation, failure recovery, independent hidden text/attachment drafts and reachable attachment removal at small sizes. Native-region clipping checks passed at 172×126, 344×252, 430×315 and 544×252: maximum PNG alpha outside the reported regions was at most 3/255.
- An isolated Windows smoke helper exercised the actual `WebHost`, `DesktopPet` and action bridge for text, attachment, recorded-WAV auto-send and voice failure/retry. The main HWND remained hidden and was never foreground during these flows. Task API responses and microphone input were test fixtures.
- Targeted native layout and office-service tests: 34/34 passed.
- Independent review findings on hidden draft/voice preservation and attachment reachability were fixed; focused regressions passed and review is clear.
- Final full `pnpm test` and `pnpm build`: passed. Existing speech-session tests also pass with synchronous composer capture and asynchronous mini WAV conversion.
- Final configure and build of `acecode-desktop` and `mini_pet_input_smoke`: passed with the refreshed embedded frontend and mini page.
- Final native rerun (`ACE_MINI_NATIVE_SMOKE=build/windows-x64-desktop-release/mini_pet_input_smoke.exe`, `node web/scripts/test-mini-background-input.mjs`): passed. It verified five accepted fixture tasks, multi-chunk attachment fidelity, automatic recording send, text and voice retry without duplicate session creation, and hidden draft/attachment preservation. The main HWND remained hidden and non-foreground throughout. Results: `%TEMP%/ace-mini-input-hTfSu2/native.log`.
- `test-mini-desktop-pet.mjs`: passed both host bridges with 20 captures, IME protection, separate voice recovery, ten-attachment scrolling at minimum size, and native-region shadow alpha checks. Captures: `%TEMP%/ace-mini-pet-DfNG5x/`.
- Strict OpenSpec validation and `git diff --check`: passed. Native macOS and a physical microphone/live model service remain outside this Windows fixture validation.

### Historical compact navigation follow-up — 2026-10-31

This estimated-size iteration is superseded by the measured shared control and independent mini scale above.

Reduced the default navigation capsule to 120×40px with 18px icons and a 14px badge; moved the activity list up by 20px to preserve its gap. The 40px input/recording height and existing user zoom remain intact. Existing browser interaction and native shadow-bound checks passed on both bridge fixtures (20 captures), and the Windows desktop rebuild passed. Independent visual review: ship. Captures are in `%TEMP%/ace-mini-size-compact/`; this size-only follow-up did not repeat native interaction tests.
