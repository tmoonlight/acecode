import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const repoRoot = path.resolve(srcRoot, '..', '..');

function source(relativePath) {
  return fs.readFileSync(path.join(repoRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('Agent Browser keeps the native webpage context menu enabled', () => {
  const host = source('src/desktop/agent_browser_host.cpp');
  assert.match(host, /put_AreDefaultContextMenusEnabled\(TRUE\)/);
  assert.doesNotMatch(host, /put_AreDefaultContextMenusEnabled\(FALSE\)/);
});

run('native document titles and favicons update matching tabs before Agent-activity filtering', () => {
  const header = source('src/desktop/agent_browser_host.hpp');
  const host = source('src/desktop/agent_browser_host.cpp');
  const desktop = source('src/desktop/main.cpp');
  const chatView = source('web/src/components/ChatView.jsx');
  const preview = source('web/src/components/PreviewDetailsPanel.jsx');
  const stateHandlerStart = chatView.indexOf('const onBrowserState = (event) =>');
  const stateHandlerEnd = chatView.indexOf('window.addEventListener(AGENT_BROWSER_STATE_EVENT', stateHandlerStart);
  const stateHandler = chatView.slice(stateHandlerStart, stateHandlerEnd);
  const metadataUpdate = stateHandler.indexOf('updateBrowserTabMetadata(prev');
  const activityGate = stateHandler.indexOf('if (!agentBrowserActivity.active || !detail.active) return;');

  assert.match(header, /kAgentBrowserDefaultTitle\[\] = u8"新标签页"/);
  assert.match(header, /std::string favicon/);
  assert.match(host, /add_DocumentTitleChanged/);
  assert.match(host, /agent_browser_favicon_expression/);
  assert.match(host, /Runtime\.evaluate/);
  assert.match(host, /favicon_generation/);
  assert.match(desktop, /\{"favicon", state\.favicon\}/);
  assert.match(preview, /<BrowserTabIcon favicon=\{tab\.favicon\}/);
  assert.match(preview, /onError=\{\(\) => setFailed\(true\)\}/);
  assert.ok(metadataUpdate >= 0, 'native title/favicon state must update the preview tab');
  assert.ok(activityGate > metadataUpdate, 'metadata updates must not be gated by live Agent activity');
});

run('Agent Browser collaboration chrome mirrors the VS Code page actions', () => {
  const panel = source('web/src/components/AgentBrowserPanel.jsx');
  const styles = source('web/src/styles/globals.css');
  const host = source('src/desktop/agent_browser_host.cpp');
  const desktop = source('src/desktop/main.cpp');

  assert.match(panel, /ace-agent-browser-share-toggle/);
  assert.match(panel, /正在与智能体共享/);
  assert.match(panel, /将元素添加到聊天/);
  assert.match(panel, /将控制台日志添加到聊天/);
  assert.match(panel, /切换开发者工具/);
  assert.doesNotMatch(panel, /ace-agent-browser-error/);
  assert.doesNotMatch(styles, /\.ace-agent-browser-error/);

  assert.match(host, /Runtime\.consoleAPICalled/);
  assert.match(host, /kAgentBrowserMaxConsoleEntries = 1000/);
  assert.match(host, /agent_browser_element_picker_expression/);
  assert.match(host, /OpenDevToolsWindow/);
  assert.match(host, /page_not_shared_with_agent/);
  assert.match(desktop, /aceDesktop_agentBrowserSetShared/);
  assert.match(desktop, /aceDesktop_agentBrowserGetConsoleLogs/);
  assert.match(desktop, /aceDesktop_agentBrowserToggleElementSelection/);
  assert.match(desktop, /aceDesktop_agentBrowserToggleDevTools/);
});

run('Browser chat contexts share the composer reference row with Pin and annotations', () => {
  const input = source('web/src/components/InputBar.jsx');
  const editorIndex = input.indexOf('<RichComposer');
  const browserReferenceIndex = input.indexOf('browserContextItems.map');
  const inlineControlsIndex = input.indexOf('const inlineContextControls');

  assert.match(input, /const browserContextItems = contextItems\.filter\(\(item\) => item\?\.type === 'browser'\)/);
  assert.match(input, /item\?\.type !== SELECTION_CONTEXT_TYPE && item\?\.type !== 'browser'/);
  assert.match(input, /<ComposerBrowserContextCard/);
  assert.ok(browserReferenceIndex > inlineControlsIndex);
  assert.ok(editorIndex > browserReferenceIndex, 'Browser references must render above the composer editor');
});

run('Agent Browser hides WebView2 blank and failure documents behind React surfaces', () => {
  const header = source('src/desktop/agent_browser_host.hpp');
  const host = source('src/desktop/agent_browser_host.cpp');
  const desktop = source('src/desktop/main.cpp');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');
  const icons = source('web/src/components/Icon.jsx');
  const browserGlobe = source('web/public/vs-icons/BrowserGlobe.svg');
  const styles = source('web/src/styles/globals.css');

  assert.match(header, /kAgentBrowserContentStateNavigationError/);
  assert.match(header, /kAgentBrowserContentStateProcessFailed/);
  assert.doesNotMatch(host, /NavigateToString/);
  assert.match(host, /add_ProcessFailed/);
  assert.match(host, /COREWEBVIEW2_PROCESS_FAILED_REASON_OUT_OF_MEMORY/);
  assert.match(
    host,
    /page->state\.content_state\s*==\s*kAgentBrowserContentStateLive/,
  );
  assert.match(desktop, /\{"content_state", state\.content_state\}/);
  assert.match(desktop, /\{"failure_kind", state\.failure_kind\}/);
  assert.match(panel, /agentBrowserShowsNativePage\(state\)/);
  assert.match(panel, /data-browser-surface=\{surface\.kind\}/);
  assert.match(panel, /无法打开此页面|agentBrowserSurfacePresentation/);
  assert.match(icons, /globe:\s*'BrowserGlobe'/);
  assert.match(browserGlobe, /<circle[^>]+r="20"/);
  assert.match(styles, /\.ace-agent-browser-status/);
  assert.match(styles, /background: var\(--ace-surface\)/);
  assert.doesNotMatch(
    styles,
    /\.ace-agent-browser-native-viewport\s*\{[^}]*background:\s*#ffffff/,
  );
});

run('application state explicitly gates the native Agent Browser surface', () => {
  const app = source('web/src/App.jsx');
  const chatView = source('web/src/components/ChatView.jsx');
  const preview = source('web/src/components/PreviewDetailsPanel.jsx');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');

  assert.match(app, /const nativeSurfacesVisible = !showSettings/);
  assert.match(app, /&& !searchOpen/);
  assert.match(app, /&& !updateDialogOpen/);
  assert.match(app, /&& !desktopCloseDialogOpen/);
  assert.match(app, /nativeSurfacesVisible=\{nativeSurfacesVisible\}/);
  assert.match(chatView, /nativeSurfacesVisible = true/);
  assert.match(chatView, /<PreviewDetailsPanel[\s\S]*nativeSurfacesVisible=\{nativeSurfacesVisible\}/);
  assert.match(preview, /surfaceEnabled=\{nativeSurfacesVisible\}/);
  assert.match(panel, /applicationVisible: surfaceEnabled/);
});

run('local overlays use declared blocking and overlap semantics', () => {
  const modal = source('web/src/components/Modal.jsx');
  const contextMenu = source('web/src/components/DesktopContextMenu.jsx');
  const findOverlay = source('web/src/components/GlobalFindOverlay.jsx');
  const imageLightbox = source('web/src/components/ImageLightbox.jsx');
  const settings = source('web/src/components/SettingsPage.jsx');
  const tokenBudget = source('web/src/components/TokenBudgetRing.jsx');
  const inputBar = source('web/src/components/InputBar.jsx');
  const message = source('web/src/components/Message.jsx');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');

  assert.match(modal, /data-ace-native-overlay="blocking"/);
  assert.match(contextMenu, /data-ace-native-overlay="overlap"/);
  assert.match(findOverlay, /data-ace-native-overlay="overlap"/);
  assert.match(imageLightbox, /data-ace-native-overlay="blocking"/);
  assert.match(settings, /data-ace-native-overlay="blocking"/);
  assert.match(tokenBudget, /ace-context-usage-panel[\s\S]*data-ace-native-overlay="overlap"/);
  assert.match(inputBar, /data-composer-capability-menu="true"[\s\S]*data-ace-native-overlay="overlap"/);
  assert.match(message, /ace-cmd-token-tip[\s\S]*data-ace-native-overlay="overlap"/);
  assert.match(modal, /notifyNativeSurfaceOverlayChange/);
  assert.match(contextMenu, /notifyNativeSurfaceOverlayChange/);
  assert.match(panel, /nativeSurfaceOverlayGeometryByDocument/);
  assert.match(panel, /nativeSurfaceOcclusionRectsFromClientRects/);
  assert.match(panel, /supportsLocalOcclusion/);
  assert.match(panel, /layout\.occlusion_rects/);
  assert.match(panel, /NATIVE_SURFACE_OVERLAY_EVENT/);
  assert.match(panel, /new MutationObserver\(scheduleLayout\)/);
  assert.match(panel, /new IntersectionObserver\(scheduleLayout\)/);
  assert.match(panel, /window\.visualViewport/);
  assert.doesNotMatch(panel, /modalIsOpen|\.ace-desktop-context-menu/);
});

run('native Agent Browser layouts reject stale revisions and unsafe windows', () => {
  const header = source('src/desktop/agent_browser_host.hpp');
  const host = source('src/desktop/agent_browser_host.cpp');
  const desktop = source('src/desktop/main.cpp');
  const webHost = source('src/desktop/web_host.cpp');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');

  assert.match(header, /std::uint64_t layout_revision = 0/);
  assert.match(header, /std::vector<AgentBrowserOcclusionRect> occlusion_rects/);
  assert.match(desktop, /value\.find\("layout_revision"\)/);
  assert.match(desktop, /value\.find\("occlusion_rects"\)/);
  assert.match(host, /bounds\.layout_revision < page->requested_bounds\.layout_revision/);
  assert.match(host, /apply_agent_browser_widget_region/);
  assert.match(host, /::SetWindowRgn/);
  assert.match(host, /::CombineRgn\(visible_region, visible_region, hole, RGN_DIFF\)/);
  assert.match(host, /page->requested_bounds\.occlusion_rects/);
  assert.match(host, /::IsWindowVisible\(parent\)/);
  assert.match(host, /!::IsIconic\(parent\)/);
  assert.match(host, /hide_agent_browser_widget\(browser_widget\)/);
  assert.match(webHost, /notify_window_visibility\(wparam != SIZE_MINIMIZED\)/);
  assert.match(desktop, /agent_browser\.set_parent_visible\(visible\)/);
  assert.match(panel, /allocateRevision: allocateAgentBrowserLayoutRevision/);
  assert.match(panel, /nextAgentBrowserLayoutRequest/);
  assert.match(panel, /syncNativeSurface\(\{ forceHidden: true, force: true \}\)/);
});
