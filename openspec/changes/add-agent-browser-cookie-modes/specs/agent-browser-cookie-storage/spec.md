# Spec Delta

## Purpose

Defines how Agent Browser keeps cookies and other site data between pages, sessions and Desktop restarts, so users can stay signed in to sites, isolate sign-ins per session, or have them discarded when Desktop quits, and can clear them on demand.

## ADDED Requirements

### Requirement: Cookie storage mode has three values with Shared as default
Agent Browser SHALL support exactly three cookie storage modes: `until_quit`, `shared` and `per_session`. When no mode has been chosen, the effective mode SHALL be `shared`. An unknown stored value SHALL be treated as `shared`.

#### Scenario: Fresh install
- **WHEN** Desktop starts and no cookie storage mode has ever been stored
- **THEN** the effective mode is `shared` and the settings UI shows Shared as selected

#### Scenario: Corrupted stored value
- **WHEN** the stored mode is a value other than the three defined ones
- **THEN** Agent Browser behaves as `shared` and the stored value is not rewritten until the user picks a mode

### Requirement: Shared mode keeps one persistent store for all sessions
In `shared` mode every Agent Browser page, regardless of owner session, SHALL use a single persistent site-data store that survives Desktop restarts. This store SHALL be the same store Agent Browser used before cookie storage modes existed, so sign-ins made before upgrading remain available.

#### Scenario: Sign-in survives restart
- **WHEN** the user signs in to a site whose sign-in cookie has an expiry, quits Desktop and starts it again in `shared` mode
- **THEN** opening that site in any session shows it signed in

#### Scenario: Upgrade keeps existing sign-ins
- **WHEN** a user who signed in to sites with a previous ACECode version upgrades and does not change the mode
- **THEN** those sites are still signed in

#### Scenario: Two sessions see the same sign-in
- **WHEN** session A signs in to a site in `shared` mode and session B then opens the same site
- **THEN** session B is signed in too

### Requirement: Until-quit mode discards site data when Desktop quits
In `until_quit` mode pages SHALL use a store whose data lives only for the current Desktop process: it SHALL survive closing and reopening pages and switching sessions, and SHALL NOT be available after Desktop restarts.

#### Scenario: Closing all pages keeps the sign-in
- **WHEN** in `until_quit` mode the user signs in, closes every Agent Browser page and opens the site again without quitting Desktop
- **THEN** the site is still signed in

#### Scenario: Restart signs out
- **WHEN** in `until_quit` mode the user signs in and restarts Desktop
- **THEN** the site is no longer signed in

### Requirement: Per-session mode isolates site data by root session
In `per_session` mode each root session SHALL have its own persistent store that survives Desktop restarts. Sub-agent sessions SHALL use their root session's store. Pages of different root sessions SHALL NOT see each other's cookies. Pages without an owner session SHALL use the `shared` store.

#### Scenario: Sessions do not see each other's sign-in
- **WHEN** in `per_session` mode session A signs in to a site and session B opens the same site
- **THEN** session B is not signed in

#### Scenario: Sub-agent uses its root session's sign-in
- **WHEN** in `per_session` mode session A signs in and a sub-agent spawned by A opens the same site
- **THEN** the sub-agent's page is signed in

#### Scenario: Per-session sign-in survives restart
- **WHEN** in `per_session` mode session A signs in, Desktop restarts and session A opens the site again
- **THEN** the site is signed in

#### Scenario: Page without owner
- **WHEN** in `per_session` mode a page is created without an owner session (for example by an older daemon)
- **THEN** the page uses the `shared` store

### Requirement: Session cookies follow browser engine lifetime
Agent Browser SHALL NOT convert cookies without an expiry into persistent cookies in any mode. Cookies without an expiry SHALL be available while Desktop runs and MAY be lost when Desktop quits, in every mode.

#### Scenario: Site uses session-only sign-in cookie
- **WHEN** a site signs the user in with a cookie that has no expiry and Desktop restarts in `shared` mode
- **THEN** the user may need to sign in again, and no setting changes this

### Requirement: Cookie storage mode is persisted and changeable from two places
The chosen mode SHALL persist across Desktop restarts. The user SHALL be able to change it from the Agent Browser toolbar menu ("保留 Cookie" with the three options and a one-line description each) and from Settings > Agent 浏览器; both places SHALL show the same current value and update when it changes elsewhere.

#### Scenario: Change from toolbar menu
- **WHEN** the user picks 按会话 in the toolbar menu and restarts Desktop
- **THEN** the effective mode is `per_session` and both the menu and the settings page show it as selected

#### Scenario: Not running in Desktop
- **WHEN** the Web UI runs in a normal browser without the Desktop bridge
- **THEN** no cookie storage controls are shown

### Requirement: Changing mode rebuilds open pages in the new store
When the mode changes, every open Agent Browser page whose store differs under the new mode SHALL be recreated in the new store, keeping its page id, owner and current URL. Pages whose store does not change SHALL NOT be reloaded.

#### Scenario: Switch from Shared to Per session with open pages
- **WHEN** session A has an open page on a site and the user switches from `shared` to `per_session`
- **THEN** the page keeps its page id and URL, is reloaded in session A's own store, and browser tools addressing that page id keep working

### Requirement: Switching to Until-quit deletes all persisted site data
Switching the mode to `until_quit` SHALL immediately delete the site data of the `shared` store and of every per-session store. Switching between `shared` and `per_session` SHALL NOT delete any data. Starting Desktop while the mode is `until_quit` SHALL delete any persisted store data left behind.

#### Scenario: Switch to Until quit
- **WHEN** the user was signed in to sites in `shared` and `per_session` stores and switches to `until_quit`
- **THEN** switching back to `shared` or `per_session` shows those sites signed out

#### Scenario: Switch between persisted modes
- **WHEN** the user switches from `shared` to `per_session` and back to `shared`
- **THEN** the sign-ins made in the `shared` store before the switch are still present

#### Scenario: Leftover data after a crash
- **WHEN** Desktop was force-terminated and the stored mode is `until_quit`
- **THEN** on the next start no previously persisted sign-in is available

### Requirement: Clear browsing data clears the store the current page uses
The toolbar menu SHALL offer "清除浏览数据". After confirmation it SHALL delete all site data (cookies, storage, cache, HTTP auth) of the store used by the current page under the current mode: the `shared` store, the current root session's store, or the until-quit store. The confirmation text SHALL state which scope will be cleared. Open pages using that store SHALL be reloaded.

#### Scenario: Clear in Shared mode
- **WHEN** in `shared` mode the user confirms "清除浏览数据"
- **THEN** every session is signed out of all sites, and the dialog said the data of all sessions would be deleted

#### Scenario: Clear in Per-session mode
- **WHEN** in `per_session` mode the user confirms "清除浏览数据" in session A
- **THEN** session A is signed out of all sites and session B's sign-ins are unchanged

#### Scenario: User cancels
- **WHEN** the user dismisses the confirmation
- **THEN** no data is deleted

### Requirement: Permanently deleting a session deletes its per-session store
When a root session is permanently deleted, its per-session store SHALL be deleted, whether or not Desktop is running at that time. Archiving a session SHALL NOT delete its store. Deleting a session SHALL NOT affect the `shared` store.

#### Scenario: Delete while Desktop runs
- **WHEN** a root session that has a per-session store is permanently deleted while Desktop runs
- **THEN** its open pages are closed and its store is deleted

#### Scenario: Delete while Desktop is not running
- **WHEN** a root session is permanently deleted from the TUI while Desktop is not running
- **THEN** its store is deleted the next time Desktop starts

#### Scenario: Archive keeps the store
- **WHEN** a session is archived and later unarchived in `per_session` mode
- **THEN** its sign-ins are still present

### Requirement: Forked sessions start with a copy of the parent's cookies
In `per_session` mode, when a session forked from another session opens its first Agent Browser page and has no store yet, its store SHALL be created with a copy of the parent session's cookies as they are at that moment. Later changes in either store SHALL NOT affect the other. If the parent has no store, the forked session SHALL start empty. Forks made while Desktop is not running SHALL also be seeded.

#### Scenario: Fork then open
- **WHEN** in `per_session` mode session A is signed in, the user forks A into C and C opens the site
- **THEN** C is signed in, and signing out in C does not sign A out

#### Scenario: Fork already has a store
- **WHEN** forked session C already has its own store
- **THEN** the parent's cookies are not copied again

### Requirement: Cookie storage works across mixed Desktop and daemon versions
Cookie storage modes SHALL NOT change the Agent Browser runtime protocol version. A daemon and a Desktop where only one side supports cookie storage modes SHALL keep working; store deletions and fork copies recorded by a newer daemon SHALL be applied the next time a supporting Desktop starts.

#### Scenario: New Desktop, old daemon
- **WHEN** a Desktop with cookie storage modes runs with a daemon from the previous release
- **THEN** browser tools work normally and pages use the store of the current mode

#### Scenario: New daemon, old Desktop
- **WHEN** a newer daemon permanently deletes a session while an older Desktop runs
- **THEN** the deletion does not fail, and the store is deleted when a Desktop supporting cookie storage modes next starts
