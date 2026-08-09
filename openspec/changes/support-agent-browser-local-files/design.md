## Context

The React address bar and Browser tools both normalize navigation inputs before the native host receives them. A second policy layer exists in WebView2 `NavigationStarting` and WKWebView's navigation delegate. All of those layers currently reject `file:`. In addition, loading a local URL is not sufficient for local web applications: WKWebView requires an explicit read-access root and Chromium file origins require a browser flag to access other local file resources.

## Goals / Non-Goals

**Goals:**

- Accept explicit `file:` URLs without an allowlist, confirmation, existence check, or workspace boundary.
- Convert `/absolute/path`, `C:\absolute\path`, and UNC paths to encoded file URLs.
- Permit a loaded local HTML page to read other local file resources on macOS and Windows.
- Keep `javascript:`, `data:`, browser-internal, and unknown schemes rejected.
- Preserve existing HTTP(S), `about:blank`, bare-host, and search behavior.

**Non-Goals:**

- Exposing ACECode native bindings to loaded pages.
- Adding a file picker or directory-listing UI.
- Making relative filesystem paths dependent on a workspace or process cwd.
- Applying the relaxed file policy to the main ACECode shell or external-link launcher.

## Decisions

### 1. Normalize local navigation in the shared runtime

The shared normalizer recognizes explicit file URLs, POSIX absolute paths, Windows drive paths, and UNC paths. Raw paths are converted to `file:` URLs with forward-slash separators and percent-encoded path bytes. Relative inputs retain the existing hostname/search behavior because they have no stable filesystem base.

The React address bar mirrors this behavior so the displayed value is immediately meaningful, while the native shared normalizer remains authoritative for both UI and tool requests.

### 2. Grant macOS file reads from the filesystem root

WKWebView file navigation uses `loadFileURL:allowingReadAccessToURL:` with `/` as the read-access root and enables `allowFileAccessFromFileURLs` on its dedicated preferences. The navigation delegate accepts `file:` and popup/file navigation reuses the same loader. No workspace check, extension filter, or confirmation is introduced.

### 3. Enable WebView2 file-origin access at environment creation

The dedicated Agent Browser WebView2 environment receives `--allow-file-access-from-files`. Direct `Navigate` and `NavigationStarting` accept normalized `file:` URLs. The flag is scoped to Agent Browser's isolated WebView2 profile and does not affect the main ACECode shell.

### 4. Preserve the content isolation boundary

Local pages receive the requested local-file visibility but still receive no ACECode bridge, daemon token, or host object. The existing authenticated desktop-to-agent proxy and page-sharing rules remain unchanged.

## Risks / Trade-offs

- [A local HTML page reads sensitive files] -> This is explicitly requested behavior; scope it to the Agent Browser profile and document the capability.
- [A malformed path is mistaken for a search] -> Cover POSIX, drive-letter, UNC, and explicit file forms with shared and frontend tests.
- [Platform navigation policies diverge] -> Add architecture assertions for both WebView2 arguments and WKWebView root read access.
- [A file URL contains spaces or Unicode] -> Percent-encode raw paths while preserving valid URL separators.

## Verification

- Run focused C++ normalization and JavaScript address tests.
- Extend macOS smoke verification to load a local HTML file and read a second local file.
- Run the full web suite/build, macOS unit/smoke targets, and Desktop build/restart.
- Retain Windows structural checks for WebView2 environment flags and navigation policy.
