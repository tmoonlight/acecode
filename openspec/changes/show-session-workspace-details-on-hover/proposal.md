## Why

Session titles in the compact desktop sidebar do not reveal which directory they operate in, so similarly named tasks are hard to distinguish without opening them. Workspace-backed sessions should expose their directory and, when applicable, current Git branch on hover while no-workspace sessions continue to reveal no workspace information.

## What Changes

- Add a compact hover/focus detail card to every workspace-backed sidebar session row, including pinned rows.
- Show the session working directory in the card.
- Show the current Git branch only when the session directory is inside a Git repository.
- Do not render or request hover details for no-workspace sessions.
- Keep the existing session-row layout, selection, pinning, archive, rename, and status behaviors unchanged.

## Capabilities

### New Capabilities

- `sidebar-session-hover-details`: Defines conditional workspace and Git context shown from compact sidebar session rows.

### Modified Capabilities

None.

## Impact

- Affects the shared session-row presentation in `web/src/components/Sidebar.jsx`, its styling, and focused frontend tests.
- Reuses session `cwd` data and the existing read-only `/api/git/info` endpoint; no daemon protocol or persistence changes are required.
- Adds no third-party dependencies.
