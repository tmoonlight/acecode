## Why

Selecting a conversation from search closes the search palette before the asynchronous session resume finishes. The underlying home or previous conversation is therefore exposed during the wait, which looks like an incorrect intermediate navigation and gives no indication that the click is still being handled.

## What Changes

- Show a full-viewport, input-blocking loading mask as soon as a session jump starts.
- Keep the mask visible while the shared resume/open flow activates a workspace, resumes the session, or hands off to a cross-workspace page load.
- Show the same transition state while a redirected page resumes its `?open=` target on startup.
- Remove the mask when the destination is ready to be shown or when navigation fails, preserving the existing error toast on failure.
- Add focused frontend coverage for the transition-state contract and accessible loading presentation.

## Capabilities

### New Capabilities

- `session-navigation-feedback`: Defines visible and accessible progress feedback for asynchronous conversation jumps.

### Modified Capabilities

None.

## Impact

- `web/src/App.jsx` shared session-jump state and rendering.
- A focused Web UI component and/or styles for the full-screen loading mask.
- Frontend architecture/unit tests; no daemon API, persistence, or dependency changes.
