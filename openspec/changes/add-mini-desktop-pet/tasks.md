# Tasks

## 1. Registration and resources

- [x] 1.1 Register mini in settings, persistence, native menus and dedicated page routing; verify preference/lifecycle and page-family tests.

## 2. Navigation surface and actions

- [x] 2.1 Build the screenshot-based capsule, activity cards, empty/disconnected states and native gestures; verify browser interactions and responsive screenshots.
- [x] 2.2 Connect new-session and voice to existing application flows; verify action dispatch, visible-composer ownership and unavailable speech behavior.

## 3. Integration

- [x] 3.1 Run web tests/build, focused native build/tests and strict OpenSpec validation; record results and platform limits in design.md.

## 4. Independent task input — latest user correction

- [x] 4.1 Replace main-window shortcuts with a local mini composer, attachment selection and compact recording/stop state; verify screenshots, IME, draft retention and shadow bounds at minimum size.
- [x] 4.2 Add a background task/recording controller using existing authenticated APIs, with duplicate guards, retry, cleanup and immediate activity feedback; verify unit and browser integration tests without a main composer.
- [x] 4.3 Allow keyboard focus in mini only while composing and remove main-window activation from input actions on Windows/macOS; verify native helpers, Windows build and a hidden-main-window smoke check where available.
- [x] 4.4 Refresh web/native assets, run relevant regressions and strict validation, and record actual validation limits.

## 5. Compact default navigation size

- [x] 5.1 Reduce the base navigation capsule from 180×60px to 120×40px with proportional icons/badge and closer activity placement; verify existing browser interaction/shadow checks and refresh the Windows build.

## 6. Shared controls and screen-aware expansion — latest corrections

- [x] 6.1 Extract the existing companion-pet bubble renderer/styles into a shared asset and use it in both classic pets and mini; verify matching markup, metrics, status/stop/read behavior and classic regressions.
- [x] 6.2 Reuse the conversation send-button definition and measured theme/size, with a separate mini default scale; verify physical size, icon and colors against the actual conversation button at native DPI.
- [x] 6.3 Expand notifications into the monitor work area without visible scrollbars; prefer below, flip above when needed, and clamp bounds while preserving toolbar position; verify placement, overflow access, drag/zoom and native click-through.
- [x] 6.4 Refresh and verify web/native builds and independent visual review; document platform limits and exact results.
