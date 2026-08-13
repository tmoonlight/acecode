## 1. Durable onboarding state

- [x] 1.1 Add a checked, serialized state-flag write path while preserving the existing `write_state_flag` API
- [x] 1.2 Add state-file unit coverage for checked success, failure, field preservation, and concurrent callers
- [x] 1.3 Add authenticated Desktop onboarding status and idempotent dismiss routes
- [x] 1.4 Add daemon route tests for unseen, dismissed, repeated dismissal, auth, and persistence failure
- [x] 1.5 Document the new endpoints in `docs/daemon-api.md`

## 2. Frontend tour foundation

- [x] 2.1 Add the preferred guided-tour dependency and update the pnpm lockfile
- [x] 2.2 Add frontend API methods for onboarding status and dismissal
- [x] 2.3 Add pure eligibility, step construction, terminal-action, and no-model helpers with unit tests
- [x] 2.4 Add an ACECode-styled guided-tour controller with localized controls, accessibility behavior, target blocking, and reduced-motion handling

## 3. Application integration

- [x] 3.1 Add stable `data-tour-target` markers to Sidebar, Home workspace selector/composer, StatusBar, and TopBar Settings
- [x] 3.2 Integrate automatic Desktop-only eligibility, deep-link settlement, overlay pause/resume, abort-without-dismiss, and transient sidebar expansion in `App.jsx`
- [x] 3.3 Add the Settings "重新查看新手指引" action and forced replay flow
- [x] 3.4 Add the no-model final action that dismisses the tour and opens Model Settings
- [x] 3.5 Add integration-focused tests for automatic start, dismissal, replay, blockers, StrictMode idempotence, target loss, and no-model routing
- [x] 3.6 Fix first-run auto-start so preparation expands a collapsed sidebar before target validation, with bounded delayed-target retries and regression coverage

## 4. Verification and size gate

- [x] 4.1 Run Web tests and production build, then record raw and gzip deltas from the 2,342,017 / 691,250 byte baseline (2,428,624 / 721,137; +86,607 raw, +29,887 gzip)
- [x] 4.2 Replace the tour dependency with Driver.js and re-verify if either 180 KiB raw or 50 KiB gzip growth limit is exceeded (not required: both React Joyride deltas passed)
- [x] 4.3 Run targeted C++ state/API tests and the relevant CTest slice (19/19 passed)
- [x] 4.4 Run Desktop visual and keyboard checks for shell/webapp modes, light/dark themes, sidebar collapse, resize, 100%/150% DPI, Escape, focus trapping, and overlay priority (shell at host 150% DPI plus mode contract tests; light/dark, transient collapse, maximize/restore, alert-dialog focus trap, Escape persistence, and search pause/resume passed)
- [x] 4.5 Run `openspec validate add-desktop-guided-tour --strict` and confirm apply instructions report all tasks complete
- [x] 4.6 Unify the guided-tour close control with ACECode's compact icon-button size, hover, and keyboard-focus styles
- [x] 4.7 Expand the tour to seven steps with explicit Add Project and New Conversation targets, copy, tests, and Desktop verification
