## MODIFIED Requirements

### Requirement: Agent Browser supports unrestricted local file navigation

ACECode Desktop Agent Browser SHALL accept explicit file URLs and absolute local filesystem paths through both the visible address bar and Browser navigation tools on macOS and Windows. Local pages SHALL be permitted to read other local file resources without workspace scoping, confirmation, extension filtering, or an allowlist.

#### Scenario: Explicit file URL is entered
- **WHEN** the user or Browser tool navigates to a valid `file:` URL
- **THEN** Agent Browser SHALL load that local URL instead of rejecting its scheme

#### Scenario: POSIX absolute path is entered
- **WHEN** the input starts with an absolute POSIX path such as `/Users/example/page.html`
- **THEN** Agent Browser SHALL convert it to an encoded `file:///Users/example/page.html` URL and load it

#### Scenario: Windows path is entered
- **WHEN** the input is a drive-letter path or UNC path
- **THEN** Agent Browser SHALL convert it to the corresponding encoded file URL and load it in WebView2

#### Scenario: Local page references another local file
- **WHEN** a loaded local page requests another `file:` resource outside its own directory
- **THEN** the request SHALL be permitted by the Agent Browser platform host

#### Scenario: Privileged executable scheme is entered
- **WHEN** the input uses `javascript:`, `data:`, a browser-internal scheme, or another unsupported scheme
- **THEN** Agent Browser SHALL continue to reject the navigation
