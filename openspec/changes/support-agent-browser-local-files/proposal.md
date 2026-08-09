## Why

Agent Browser currently rejects `file:` URLs and treats absolute local paths as web searches or hostnames. This prevents the visible Desktop Browser and Browser tools from opening generated HTML, documentation, images, and other local artifacts. The limitation exists in shared normalization plus platform-specific navigation policy, so both macOS and Windows must change together.

## What Changes

- Accept `file:` URLs in the shared Agent Browser navigation contract.
- Convert absolute POSIX paths, Windows drive paths, and UNC paths into canonical file URLs.
- Allow macOS WKWebView local pages to read resources from any local file URL.
- Enable unrestricted file-to-file access in the Windows WebView2 Agent Browser environment.
- Update the address bar, Browser tool descriptions, tests, smoke coverage, and documentation.

## Capabilities

### Modified Capabilities

- `agent-browser-ui`: the Desktop address bar accepts local paths and file URLs.
- `agent-browser-tools`: `browser_open` and `browser_navigate` accept local paths and file URLs.
- `macos-agent-browser`: WKWebView loads local pages with root-level file read access.
- `windows-agent-browser`: WebView2 enables file-to-file access for local pages.

## Impact

- Shared URL normalization under `src/desktop/agent_browser_runtime.*` and `web/src/lib/agentBrowser.js`.
- Platform hosts under `src/desktop/agent_browser_host.cpp` and `src/desktop/agent_browser_host_mac.mm`.
- Browser tool schema text under `src/tool/agent_browser/browser_tools.cpp`.
- Agent Browser tests, smoke helpers, and `docs/agent-browser.md`.
