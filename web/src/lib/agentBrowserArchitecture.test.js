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
  const macHost = source('src/desktop/agent_browser_host_mac.mm');
  assert.match(host, /put_AreDefaultContextMenusEnabled\(TRUE\)/);
  assert.doesNotMatch(host, /put_AreDefaultContextMenusEnabled\(FALSE\)/);
  assert.match(macHost, /addEventListener\('contextmenu', suppress, true\)/);
  assert.match(macHost, /removeEventListener\('contextmenu', suppress, true\)/);
});

run('Agent Browser grants local file navigation on macOS and Windows', () => {
  const host = source('src/desktop/agent_browser_host.cpp');
  const macHost = source('src/desktop/agent_browser_host_mac.mm');
  const runtime = source('src/desktop/agent_browser_runtime.cpp');

  assert.match(host, /--allow-file-access-from-files/);
  assert.match(macHost, /\[scheme_value isEqualToString:@"file"\]/);
  assert.match(macHost, /forKey:@"allowFileAccessFromFileURLs"/);
  assert.match(macHost, /fileURLWithPath:@"\/" isDirectory:YES/);
  assert.match(macHost, /loadFileURL:request\.URL[\s\S]*allowingReadAccessToURL:root_read_access/);
  assert.match(runtime, /windows_drive_path\(value\) \|\| unc_path\(value\)/);
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

run('Agent Browser mouse-producing tools show one non-blocking AI pointer at exact input coordinates', () => {
  const header = source('src/tool/agent_browser/browser_tools.hpp');
  const tools = source('src/tool/agent_browser/browser_tools.cpp');
  const pointerOverlay = source('src/tool/agent_browser/pointer_overlay.cpp');
  const click = tools.slice(
    tools.indexOf('ToolImpl click_tool()'),
    tools.indexOf('ToolImpl fill_tool()'),
  );
  const hover = tools.slice(
    tools.indexOf('ToolImpl hover_tool()'),
    tools.indexOf('ToolImpl drag_tool()'),
  );
  const scroll = tools.slice(
    tools.indexOf('ToolImpl scroll_tool()'),
    tools.indexOf('ToolImpl wait_tool()'),
  );
  const drag = tools.slice(
    tools.indexOf('ToolImpl drag_tool()'),
    tools.indexOf('ToolImpl scroll_tool()'),
  );
  const evaluate = tools.slice(
    tools.indexOf('ToolImpl evaluate_tool()'),
    tools.indexOf('ToolImpl close_tool()'),
  );
  const keyboardAndFocus = tools.slice(
    tools.indexOf('ToolImpl fill_tool()'),
    tools.indexOf('ToolImpl hover_tool()'),
  );

  assert.match(header, /std::string agent_browser_pointer_script\(/);
  assert.match(header, /agent_browser_evaluate_pointer_observer_script\(bool install\)/);
  assert.match(tools, /kPointerCommandTimeout = std::chrono::seconds\(2\)/);
  assert.match(pointerOverlay, /attachShadow\(\{mode:'open'\}\)/);
  assert.match(pointerOverlay, /:host\{all:initial\}/);
  assert.doesNotMatch(pointerOverlay, /:host\{all:initial!important\}/);
  assert.match(pointerOverlay, /'pointer-events':'none'/);
  assert.match(pointerOverlay, /const critical=\{display:'block',position:'fixed'/);
  assert.doesNotMatch(pointerOverlay, /const critical=\{all:/);
  assert.match(pointerOverlay, /host\.removeAttribute\('style'\)/);
  assert.match(pointerOverlay, /badge\.textContent='AI'/);
  assert.match(pointerOverlay, /prefers-reduced-motion:reduce/);
  assert.match(pointerOverlay, /event\.isTrusted!==false/);
  assert.match(pointerOverlay, /event\.clientX/);
  assert.match(pointerOverlay, /zeroOutsideTarget/);
  assert.match(pointerOverlay, /globalThis\.addEventListener/);
  assert.match(pointerOverlay, /globalThis\.removeEventListener/);
  assert.match(tools, /std::string pointer_error/);
  assert.equal((tools.match(/show_agent_pointer\(/g) || []).length, 6);

  assert.match(click, /show_agent_pointer\(client, x, y, "click", context\)/);
  assert.ok(click.indexOf('show_agent_pointer') < click.indexOf('dispatch_mouse'));
  assert.match(hover, /show_agent_pointer\([\s\S]*"hover",[\s\S]*context\)/);
  assert.ok(hover.indexOf('show_agent_pointer') < hover.indexOf('dispatch_mouse'));
  assert.match(scroll, /show_agent_pointer\(client, x, y, "scroll", context\)/);
  assert.ok(scroll.indexOf('show_agent_pointer') < scroll.indexOf('dispatch_mouse'));
  assert.match(drag, /show_agent_pointer\(client, x1, y1, "click", context\)/);
  assert.match(drag, /show_agent_pointer\(client, x2, y2, "drag", context\)/);
  assert.match(drag, /step <= 8/);
  assert.ok(drag.indexOf('show_agent_pointer(client, x1') < drag.indexOf('"mousePressed"'));
  assert.ok(drag.indexOf('show_agent_pointer(client, x2') < drag.indexOf('step <= 8'));
  assert.match(evaluate, /set_evaluate_pointer_observer\(client, true, context\)/);
  assert.match(evaluate, /set_evaluate_pointer_observer\(client, false, context\)/);
  assert.ok(evaluate.indexOf('true, context') < evaluate.indexOf('json value = evaluate'));
  assert.ok(evaluate.indexOf('json value = evaluate') < evaluate.indexOf('false, context'));
  assert.doesNotMatch(keyboardAndFocus, /show_agent_pointer|set_evaluate_pointer_observer/);
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
  const coordinator = source('web/src/lib/agentBrowserSurfaceCoordinator.js');

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
  assert.match(panel, /nativeSurfaceSupportsLocalOcclusion/);
  assert.match(panel, /desiredLayout\.occlusion_rects/);
  assert.match(coordinator, /os === 'windows' \|\| os === 'macos'/);
  assert.match(coordinator, /\[role="menu"\]/);
  assert.match(coordinator, /NATIVE_SURFACE_IMPLICIT_OVERLAY_SELECTOR/);
  assert.match(panel, /NATIVE_SURFACE_OVERLAY_EVENT/);
  assert.match(panel, /new MutationObserver\(scheduleLayout\)/);
  assert.match(panel, /new IntersectionObserver\(scheduleLayout\)/);
  assert.match(panel, /window\.visualViewport/);
  assert.doesNotMatch(panel, /modalIsOpen|\.ace-desktop-context-menu/);
});

run('every current floating-surface owner participates in the native overlay contract', () => {
  const floatingSurfaceOwners = [
    'ChangeReview.jsx',
    'ChatView.jsx',
    'ComposerSessionControls.jsx',
    'ConsoleDock.jsx',
    'ConversationTurnScrubber.jsx',
    'DesktopContextMenu.jsx',
    'GitChangesPanel.jsx',
    'GitSessionPill.jsx',
    'GlobalFindOverlay.jsx',
    'ImageLightbox.jsx',
    'InputBar.jsx',
    'LoopPage.jsx',
    'Message.jsx',
    'Modal.jsx',
    'PathReferenceDropdown.jsx',
    'PreviewDetailsPanel.jsx',
    'SearchPalette.jsx',
    'SelectionActionPopover.jsx',
    'SelectionAnnotationBadge.jsx',
    'SelectionAnnotationOverlay.jsx',
    'SessionContentLoading.jsx',
    'SessionNavigationMask.jsx',
    'SettingsPage.jsx',
    'Sidebar.jsx',
    'SlashDropdown.jsx',
    'Toast.jsx',
    'TokenBudgetRing.jsx',
    'TopBar.jsx',
  ];
  for (const file of floatingSurfaceOwners) {
    assert.match(
      source(`web/src/components/${file}`),
      /data-ace-native-overlay="(?:overlap|blocking)"/,
      file,
    );
  }
});

run('native Agent Browser layouts reject stale revisions and unsafe windows', () => {
  const header = source('src/desktop/agent_browser_host.hpp');
  const host = source('src/desktop/agent_browser_host.cpp');
  const macHost = source('src/desktop/agent_browser_host_mac.mm');
  const desktop = source('src/desktop/main.cpp');
  const webHost = source('src/desktop/web_host.cpp');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');
  const desktopCmake = source('cmake/acecode_desktop.cmake');
  const testCmake = source('tests/CMakeLists.txt');

  assert.match(header, /std::uint64_t layout_revision = 0/);
  assert.match(header, /std::vector<AgentBrowserOcclusionRect> occlusion_rects/);
  assert.match(desktop, /value\.find\("layout_revision"\)/);
  assert.match(desktop, /value\.find\("occlusion_rects"\)/);
  assert.match(desktop, /"occlusion_rect_count"/);
  assert.match(host, /bounds\.layout_revision < page->requested_bounds\.layout_revision/);
  assert.match(host, /apply_agent_browser_widget_region/);
  assert.match(host, /::SetWindowRgn/);
  assert.match(host, /::CombineRgn\(visible_region, visible_region, hole, RGN_DIFF\)/);
  assert.match(host, /page->requested_bounds\.occlusion_rects/);
  assert.match(macHost, /ACECodeAgentBrowserSurfaceView/);
  assert.match(macHost, /CAShapeLayer\* mask/);
  assert.match(macHost, /\[target_view isFlipped\]/);
  assert.match(macHost, /\? top[\s\S]*: height - top - NSHeight\(rect\)/);
  assert.match(macHost, /\[subview_layer setMask:subview_mask\]/);
  assert.match(macHost, /kCAFillRuleEvenOdd/);
  assert.match(macHost, /NSPointInRect\(local, \[value rectValue\]\)/);
  assert.match(macHost, /\[surface_layer setMask:nil\]/);
  assert.match(macHost, /bounds\.layout_revision < page->requested_bounds\.layout_revision/);
  assert.match(macHost, /bounds\.occlusion_rects/);
  assert.match(desktopCmake, /-framework QuartzCore/);
  assert.match(testCmake, /-framework QuartzCore/);
  assert.match(host, /::IsWindowVisible\(parent\)/);
  assert.match(host, /!::IsIconic\(parent\)/);
  assert.match(host, /hide_agent_browser_widget\(browser_widget\)/);
  assert.match(webHost, /notify_window_visibility\(wparam != SIZE_MINIMIZED\)/);
  assert.match(desktop, /agent_browser\.set_parent_visible\(visible\)/);
  assert.match(panel, /allocateRevision: allocateAgentBrowserLayoutRevision/);
  assert.match(panel, /nextAgentBrowserLayoutRequest/);
  assert.match(panel, /failedNativeSurfaceOcclusionSignature/);
  assert.match(panel, /nativeSurfaceLayoutWithOcclusionFallback/);
  assert.match(panel, /syncNativeSurface\(\{ forceHidden: true, force: true \}\)/);
});
