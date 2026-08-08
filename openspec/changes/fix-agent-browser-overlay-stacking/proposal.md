## Why

The Desktop Agent Browser is a native WebView layered above the main React WebView. CSS stacking contexts cannot cross that native-view boundary, so ACECode menus, popovers, toasts, and dialogs can be covered by the Browser even when their CSS z-index is higher. The existing local-occlusion path is not observable when delivery fails and only recognizes explicitly enumerated overlays, leaving the user-facing stacking guarantee unreliable.

## What Changes

- Define one document-level overlay contract for all transient ACECode floating surfaces that need to appear above the native Agent Browser.
- Make explicitly registered overlap surfaces authoritative even when the Browser placeholder wins DOM hit testing.
- Acknowledge native layout updates and fail closed: if a layout containing local occlusions is rejected, temporarily hide the Browser so the floating surface remains usable.
- Harden the macOS native surface mask and hit-testing behavior for multiple simultaneous floating surfaces.
- Add behavioral and macOS smoke coverage for the complete overlay contract.

## Capabilities

### Modified Capabilities

- `agent-browser-ui`: ACECode floating surfaces consistently render and receive input above the visible native Browser on supported Desktop platforms.

## Impact

- Overlay discovery and layout coordination under `web/src/lib/agentBrowserSurfaceCoordinator.js`.
- Agent Browser layout delivery under `web/src/components/AgentBrowserPanel.jsx` and `web/src/lib/agentBrowser.js`.
- Shared floating-surface markup under `web/src/components/`.
- macOS native view compositing under `src/desktop/agent_browser_host_mac.mm`.
- Focused JavaScript and macOS Desktop smoke tests.
