<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

The current appearance implementation has three independent React/localStorage owners: `ThemeProvider` stores `ace.theme` and `ace.colorTheme`, while `App` stores `fontSize` inside `acecode.uiPrefs.v1`. This works only while the WebUI keeps the same origin. Desktop-managed daemons are assigned available loopback ports, and the port participates in the browser origin, so a later Desktop process cannot see the previous origin's localStorage.

ACECode already exposes authenticated `GET/PUT /api/config/ui-preferences`, backed by the stable user `config.json`, but that endpoint currently retains only a disabled avatar compatibility field. Desktop also loads the same user configuration before creating the WebView and already injects locale bootstrap state before frontend modules run. Those are the existing seams for durable and flash-free appearance restoration.

The affected files already contain unrelated work, so implementation must use narrow patches and retain existing storage keys and public theme APIs.

## Goals / Non-Goals

**Goals:**

- Make light/dark mode, blue/orange color theme, and small/medium/large font size survive Desktop daemon port changes and process restarts.
- Restore the configured appearance before the first Desktop React render and confirm it from the authenticated daemon after connection.
- Keep every existing appearance control live, including the TopBar light/dark button and Settings controls.
- Preserve old browser caches and older configuration files without migration failures.
- Reject invalid API/config values and roll the visible UI back to the last confirmed state when a save fails.

**Non-Goals:**

- Persist unrelated panel/layout preferences in this change.
- Add TUI theme persistence, new palettes, new font-size choices, or OS theme tracking after startup.
- Synchronize preferences between different machines or user profiles.
- Replace the existing CSS token/theme architecture.

## Decisions

### Use `web_ui` config as the canonical store

`WebUiPreferencesConfig` will add `theme`, `color_theme`, and `font_size`. Canonical values are `system|light|dark`, `blue|orange`, and `small|medium|large`; defaults are `system`, `blue`, and `medium`. The existing `show_acecode_avatar` compatibility field remains normalized to `false`.

This reuses the existing authenticated UI-preferences endpoint and stable user config instead of creating another file or a theme-specific route. `system` preserves the historical first-run system-color fallback until the user explicitly changes light/dark mode.

### Keep localStorage as a cache, not the authority

Existing keys remain unchanged. Their values provide a fast same-origin fallback and continue to support ordinary browser use if the daemon is temporarily unavailable. Once an authenticated GET returns a complete appearance payload, the frontend applies that payload and the existing hooks refresh their local caches.

This avoids destructive key migration and permits rollback to older versions, which will continue to ignore the new config fields.

### Bootstrap Desktop from native configuration

Desktop will inject one JSON-safe `window.__ACECODE_APPEARANCE__` object in the existing pre-navigation init script. `ThemeProvider` and `App` use that object as their default before falling back to system/default values. Existing localStorage still wins when it is valid for the current origin, and the later authenticated GET resolves any stale cache.

This follows the locale bootstrap pattern and avoids a visible default-theme frame on every new random port. A frontend-only GET would eventually restore the setting but would still render the wrong first frame.

### Persist complete appearance snapshots with serialized writes

All appearance entry points route through an App-owned mutation function. It applies the requested field immediately, serializes complete preference snapshots through the existing PUT endpoint, and tracks the latest confirmed snapshot. Writes are queued to prevent reordered network responses from overwriting a newer user choice. A failed latest write restores the last confirmed snapshot and shows an error toast.

The TopBar receives the same persisted toggle callback as Settings. This prevents the top-level black/white shortcut from bypassing the new persistence path.

### Extend the existing PUT endpoint compatibly

PUT accepts one or more recognized UI preference fields, validates every supplied field, preserves omitted fields, and returns the complete normalized preference object. Legacy `{show_acecode_avatar:boolean}` calls remain valid and still normalize that field to `false`. Frontend appearance writes include the legacy field and require the response to contain the complete appearance contract before considering a save confirmed.

## Risks / Trade-offs

- [Multiple daemon processes can hold older in-memory config snapshots] -> Each newly started daemon loads the stable file, frontend sends complete appearance snapshots, and authenticated restore re-applies the canonical stored values after navigation.
- [An older daemon implements only the avatar endpoint] -> Treat an incomplete response as unsupported persistence, keep the existing cache available, and roll back rather than falsely reporting success.
- [Rapid clicks can reorder PUT completion] -> Serialize writes and only let the latest revision update or roll back visible state.
- [Invalid legacy config values exist] -> Validate each field independently, log and retain the default for invalid values, and write only normalized values.
- [Desktop bootstrap and daemon response can differ after external config edits] -> Use bootstrap for first paint, then let the authenticated GET become authoritative.

## Migration Plan

1. Add sparse config parsing/serialization and expand the existing endpoint while retaining old defaults and payload compatibility.
2. Add Desktop bootstrap injection and frontend parsing with localStorage fallback.
3. Route TopBar and Settings mutations through the persistent writer.
4. Existing users need no explicit migration; missing fields resolve to system/blue/medium and are written only after a non-default choice or API mutation.
5. Rollback is safe because older builds ignore the new `web_ui` fields and continue using the existing localStorage keys.

## Open Questions

(none)
