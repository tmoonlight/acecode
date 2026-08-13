## Why

ACECode Desktop currently opens directly into the full application without teaching first-time users how projects, the home composer, model and permission controls, history, and settings fit together. A short, dismissible guided tour can make the first useful action obvious without permanently blocking experienced users, but its completion state must survive Desktop's dynamic loopback ports and Edge fallback profiles.

## What Changes

- Add a seven-step Desktop-only guided tour with a global mask, spotlighted controls, localized navigation, progress, keyboard support, and an always-available close path.
- Show the tour automatically only when the current guide version has not been dismissed and the normal Desktop home surface is ready; defer it while deep links, sessions, or higher-priority overlays are active.
- Enter a transient preparation state before checking target readiness so a stored collapsed-sidebar preference and slightly delayed Home targets cannot suppress the first-run tour.
- Persist dismissal for the current guide version in ACECode runtime state through an authenticated daemon API so WebView and Edge compatibility modes behave consistently.
- Add a Settings action that replays the tour without clearing its persisted dismissal marker.
- Adapt the final step when no model is configured by taking the user to model settings after the tour closes.
- Use a maintained third-party React tour library, but require measured production bundle growth and reject the dependency if the final increment is disproportionate to the feature.

## Capabilities

### New Capabilities

- `desktop-guided-tour`: First-run eligibility, seven-step tour behavior, dismissal persistence, replay, accessibility, overlay coordination, and package-size acceptance.

### Modified Capabilities

None.

## Impact

- Web UI: `web/src/App.jsx`, the top bar, sidebar, home composer, status bar, Settings, shared styles, API client, and focused unit tests.
- Daemon/runtime state: a small authenticated onboarding status endpoint and durable versioned dismissal flag in `~/.acecode/state.json`.
- Dependencies: one React-compatible guided-tour package plus its lockfile changes; production bundle growth will be measured before acceptance.
- Documentation: daemon API reference and user-facing Settings/replay behavior.
