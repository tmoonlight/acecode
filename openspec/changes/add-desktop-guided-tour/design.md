## Context

ACECode Desktop renders the React web UI from a daemon served on a dynamically selected loopback port. The embedded shell has native bridges, while Edge compatibility mode is identified by `ace_webapp=1` and does not have those bridges. Existing browser preferences therefore cannot reliably represent a once-per-install guided-tour dismissal: the port changes the origin and Edge fallback uses a clean per-launch profile.

The home surface, project sidebar, status bar, and Settings entry are already owned by separate components below `App.jsx`. The app also has higher-priority permission, question, search, and Settings overlays. The implementation must coordinate those surfaces, keep the tour accessible, and avoid materially increasing the single-file production bundle.

Baseline production build before this change:

- `web/dist/index.html`: 2,342,017 bytes raw
- gzip of `web/dist/index.html`: 691,250 bytes

## Goals / Non-Goals

**Goals:**

- Teach the normal first-run workflow in seven steps without allowing the tour to interfere with real task execution.
- Persist dismissal across WebView ports, restarts, and Edge compatibility profiles.
- Allow replay from Settings without resetting the persisted marker.
- Preserve keyboard and assistive-technology usability.
- Keep production growth bounded and measured.

**Non-Goals:**

- No multi-page or session-specific tutorial.
- No analytics for distinguishing completed versus skipped tours.
- No browser-direct automatic tour.
- No cross-device synchronization.
- No change to existing localStorage UI-preference schemas.

## Decisions

### D1. Use a third-party tour engine behind an ACECode-owned controller

Use `react-joyride@3.2.0` first. `App.jsx` owns eligibility, run/pause, forced replay, blocking overlays, and dismissal persistence; Joyride owns the current step and positioning. ACECode supplies localized steps, stable targets, styling, and event handling.

This is preferred over a custom spotlight because focus trapping, viewport collision handling, SVG masking, target-not-found reporting, and cleanup are correctness work rather than product differentiation. Shepherd is excluded by its AGPL license. Driver.js remains the fallback if the package-size gate fails.

### D2. Enforce a measured package-size gate

After implementation, compare the single-file production build with the recorded baseline. The selected dependency is accepted only when growth is no more than both:

- 184,320 raw bytes (180 KiB)
- 51,200 gzip bytes (50 KiB)

If React Joyride exceeds either limit, remove it, implement the same contract with Driver.js, and re-run the measurement. The measured delta is recorded in the final verification output and in the OpenSpec task result.

### D3. Use seven stable, non-interactive steps

Add unique `data-tour-target` markers for:

1. Sidebar: projects, history, and extension entry points.
2. Sidebar Add Project button: add an existing local directory to the project list.
3. Top-bar New Conversation button: start a new task without implying that a project directory is created.
4. Home workspace selector: task scope.
5. Home composer: natural-language tasks and slash commands.
6. Status bar: model and permission controls.
7. Top-bar Settings button: configuration and replay.

The tour blocks pointer interaction with highlighted targets. If no model is configured, the final action dismisses the guide and opens the Models Settings section. During a tour, the effective sidebar is expanded through a transient override; the stored preference is not changed.

### D4. Define one versioned dismissal terminal state

Close, Escape, Skip, and completing the last step all mean that the current guide version is dismissed and will not auto-run again. Unexpected teardown or navigation away does not persist dismissal. Replay ignores the persisted marker but does not clear it.

The backend owns `guide_version = 1` and the versioned state flag `desktop_guided_tour_v1_dismissed`. A future guide only increments this version for a meaningful workflow change, not for every ACECode release.

### D5. Persist through an authenticated daemon API

Add:

- `GET /api/ui/onboarding/desktop` → `{ "guide_version": 1, "dismissed": boolean }`
- `POST /api/ui/onboarding/desktop/dismiss` → the same current state

Both routes require existing daemon authentication. The POST is idempotent. Runtime state, not `config.json`, stores the marker because `state_file.hpp` explicitly owns once-only prompt flags. State-file read-modify-write operations are serialized, and a new checked write API reports persistence failure so the route can return `PERSIST_FAILED` instead of claiming success.

### D6. Prepare the settled Desktop home before requiring targets

Automatic start requires all of the following:

- UI mode is embedded Desktop shell or Edge webapp compatibility mode.
- Authentication and onboarding-state reads succeeded.
- Startup deep-link navigation has settled.
- No active session is open.
- Settings, search, permission, and question overlays are closed.

After these logical conditions pass, the controller enters its transient
preparation state first. That state expands a stored collapsed sidebar without
changing the preference. The controller then performs a short bounded retry for
the required target elements before setting `run=true`; a failed probe aborts
without persisting dismissal. Target presence is deliberately not part of the
preparation eligibility predicate because some targets cannot mount until the
preparation state expands the sidebar.

State-read failure is fail-closed: the app remains usable and does not show the guide. A deep link defers the guide until the user later returns Home. Closing to tray preserves the mounted tour; a full reload restarts an undismissed tour from step one.

### D7. Coordinate overlays and accessibility explicitly

The tour renders at the document body with a top-level z-index and blocks target interaction and context menus. A higher-priority business overlay pauses the tour; after it closes, the current target is revalidated. Required-target loss aborts the run without dismissal.

Keep the library's alert-dialog semantics, focus trap, and focus restoration. ACECode maps Escape to the same dismissal path only while no business overlay is active, provides Chinese accessible labels, and removes tour animation/automatic smooth scrolling under `prefers-reduced-motion`.

## Risks / Trade-offs

- **[React Joyride exceeds the bundle gate]** → Replace it with Driver.js before accepting the change; behavior and tests remain library-neutral.
- **[Deep-link and eligibility effects race]** → Track startup navigation settlement explicitly, prepare the Home layout, and retry target discovery for a bounded interval before setting `run=true`.
- **[A target disappears during a step]** → Abort without writing dismissal so a later normal Home entry can retry.
- **[State persistence fails]** → Close the current visual tour, report the failure, and allow it to appear again next launch.
- **[Tour competes with permission or question UI]** → Pause the tour and let the business overlay exclusively own focus and Escape.
- **[Cross-platform state writers overlap]** → Serialize the shared state-file read-modify-write sequence rather than relying only on atomic rename.

## Migration Plan

1. Add the state API and checked persistence without changing existing state keys.
2. Add the frontend controller and stable target attributes.
3. Add the dependency and measure the production delta against the recorded baseline.
4. If the gate fails, replace only the tour engine and keep the controller/API contract.
5. Rollback removes the UI/controller and dependency; the inert versioned state flag may remain safely in `state.json`.

## Open Questions

None. Real WebView DPI placement and screen-reader behavior remain implementation verification items rather than architecture decisions.
