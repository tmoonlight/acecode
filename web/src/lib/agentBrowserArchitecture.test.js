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

run('macOS Agent Browser exposes NSError diagnostics and keeps system authentication handling', () => {
  const header = source('src/desktop/agent_browser_host.hpp');
  const macHost = source('src/desktop/agent_browser_host_mac.mm');
  const desktop = source('src/desktop/main.cpp');
  const surface = source('web/src/lib/agentBrowserSurface.js');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');
  const styles = source('web/src/styles/globals.css');
  const authStart = macHost.indexOf('didReceiveAuthenticationChallenge:');
  const authEnd = macHost.indexOf('webViewWebContentProcessDidTerminate:', authStart);
  const authHandler = macHost.slice(authStart, authEnd);

  assert.match(header, /std::string diagnostic/);
  assert.match(desktop, /\{"diagnostic", state\.diagnostic\}/);
  assert.match(macHost, /native_error_details\(NSError\* error/);
  assert.match(macHost, /native_error_diagnostic\(NSError\* error\)/);
  assert.match(macHost, /"domain"/);
  assert.match(macHost, /"code"/);
  assert.match(macHost, /"description"/);
  assert.match(macHost, /"failing_url"/);
  assert.match(macHost, /"underlying"/);
  assert.match(macHost, /NSURLErrorAppTransportSecurityRequiresSecureConnection/);
  assert.match(macHost, /NSURLErrorUserAuthenticationRequired/);
  assert.match(macHost, /NSURLErrorSecureConnectionFailed/);

  assert.ok(authStart >= 0 && authEnd > authStart);
  assert.match(authHandler, /authentication_challenge/);
  assert.match(
    authHandler,
    /completionHandler\(NSURLSessionAuthChallengePerformDefaultHandling, nil\)/,
  );
  assert.match(authHandler, /NSURLAuthenticationMethodServerTrust/);
  assert.match(
    authHandler,
    /completionHandler\(NSURLSessionAuthChallengeUseCredential, credential\)/,
  );
  assert.match(authHandler, /credentialForTrust:\[protection_space serverTrust\]/);
  for (const field of [
    'authentication_method',
    'host',
    'port',
    'realm',
    'is_proxy',
    'previous_failure_count',
    'proposed_credential_present',
    'perform_default_handling',
  ]) {
    assert.match(macHost, new RegExp(field));
  }
  for (const event of [
    'navigation_requested',
    'navigation_policy',
    'navigation_started',
    'navigation_redirected',
    'navigation_committed',
    'navigation_finished',
    'navigation_failed',
    'web_content_process_terminated',
  ]) {
    assert.match(macHost, new RegExp(event));
  }

  assert.match(surface, /diagnostic = String\(state\.diagnostic/);
  assert.match(panel, /ace-agent-browser-status-diagnostic/);
  assert.match(panel, /aria-label="NSError"/);
  assert.match(styles, /\.ace-agent-browser-status-diagnostic/);
  assert.match(styles, /overflow:\s*auto/);
  assert.match(styles, /user-select:\s*text/);
});

run('macOS Agent Browser is permanently development-permissive', () => {
  const macHost = source('src/desktop/agent_browser_host_mac.mm');
  const plist = source('cmake/macos/ACECodeDesktopInfo.plist.in');
  const authStart = macHost.indexOf('didReceiveAuthenticationChallenge:');
  const authEnd = macHost.indexOf('webViewWebContentProcessDidTerminate:', authStart);
  const authHandler = macHost.slice(authStart, authEnd);

  // ATS 例外只解除 WKWebView Web 内容的限制。全局 NSAllowsArbitraryLoads 会把
  // Provider / MCP / daemon / 升级器的 URLSession 一起放宽,越出 Agent Browser 边界。
  assert.match(plist, /<key>NSAppTransportSecurity<\/key>/);
  assert.match(plist, /<key>NSAllowsArbitraryLoadsInWebContent<\/key>\s*<true\/>/);
  assert.doesNotMatch(plist, /<key>NSAllowsArbitraryLoads<\/key>/);

  // 服务器信任挑战无条件接受;凭据类挑战继续交给系统,ACECode 不编造凭据。
  assert.match(authHandler, /NSURLSessionAuthChallengeUseCredential/);
  assert.match(
    authHandler,
    /completionHandler\(NSURLSessionAuthChallengePerformDefaultHandling, nil\)/,
  );
  // 系统仍能协商的旧 TLS 一律继续。
  assert.match(macHost, /shouldAllowDeprecatedTLS:\(void \(\^\)\(BOOL\)\)decisionHandler/);
  assert.match(macHost, /decisionHandler\(YES\)/);
  // WebKit 欺诈网站警告页在开发浏览器里也是挡板。
  assert.match(macHost, /setFraudulentWebsiteWarningEnabled:/);
  assert.match(macHost, /fraudulentWebsiteWarningEnabled = NO/);
  // 外部 SSO scheme 静默交给操作系统,javascript:/data: 不参与交接。
  assert.match(macHost, /external_handoff_candidate_url/);
  assert.match(macHost, /URLForApplicationToOpenURL:url/);
  assert.match(macHost, /@"javascript", @"data", @"about", @"blob"/);
});

run('Agent Browser derives navigation state from the final unhandled result', () => {
  const header = source('src/desktop/agent_browser_navigation_state.hpp');
  const state = source('src/desktop/agent_browser_navigation_state.cpp');
  const host = source('src/desktop/agent_browser_host.cpp');
  const macHost = source('src/desktop/agent_browser_host_mac.mm');

  // 中间回调的判定收敛在无平台依赖的纯逻辑层,两端共用同一套规则。
  assert.match(header, /class AgentBrowserNavigationTracker/);
  for (const decision of [
    'kIgnore',
    'kHoldPending',
    'kRetryOnce',
    'kRestorePrevious',
    'kPublishFailure',
    'kPublishSuccess',
  ]) {
    assert.match(header, new RegExp(decision));
  }
  // 重试标记必须跨过重试导航自己的 begin_navigation,否则证书始终失败的站点会
  // 无限重试。
  assert.match(state, /carry_retry_marker_/);
  assert.match(state, /certificate_retry_used_ = carry_retry_marker_/);

  // 两端都把决策交给 tracker,并在页面关闭后忽略在途回调。
  assert.match(host, /current->navigation\.on_navigation_completed\(/);
  assert.match(host, /page->navigation\.close\(\)/);
  assert.match(macHost, /page->navigation\.on_navigation_completed\(/);
  assert.match(macHost, /page->navigation\.close\(\)/);
  // macOS 的 -999 恢复导航前状态而不是发布失败。
  assert.match(macHost, /classify_darwin_navigation_failure/);
  assert.match(macHost, /kRestorePrevious/);
  assert.match(macHost, /content_state_before_navigation/);
});

run('Windows Agent Browser disables WebView2 barriers before the first navigation', () => {
  const host = source('src/desktop/agent_browser_host.cpp');
  const installStart = host.indexOf('void install_events(');
  const navigationStarting = host.indexOf('add_NavigationStarting', installStart);
  const preNavigation = host.slice(installStart, navigationStarting);

  // SmartScreen 信誉检查与内建错误文档都是挡板。
  assert.match(host, /put_IsReputationCheckingRequired\(FALSE\)/);
  assert.match(host, /put_IsBuiltInErrorPageEnabled\(FALSE\)/);
  assert.match(host, /ICoreWebView2Settings8/);

  // 证书事件必须在任何导航之前注册,否则第一条 TLS 拦截页已经露出来了。
  assert.match(preNavigation, /install_permissive_certificate_events\(page\)/);
  assert.match(host, /add_ServerCertificateErrorDetected/);
  assert.match(host, /remove_ServerCertificateErrorDetected/);
  assert.match(
    host,
    /COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW/,
  );
  assert.doesNotMatch(
    host,
    /COREWEBVIEW2_SERVER_CERTIFICATE_ERROR_ACTION_(CANCEL|DEFAULT)/,
  );

  // 外部 URI scheme 走系统 shell,并注销 token。
  assert.match(host, /add_LaunchingExternalUriScheme/);
  assert.match(host, /remove_LaunchingExternalUriScheme/);
  assert.match(host, /ICoreWebView2_18/);
  assert.match(host, /ShellExecuteW/);

  // 缺接口时如实记录能力缺失,不引入用户可见的安全模式或回退开关。
  assert.match(host, /lacks ICoreWebView2_14/);
  assert.match(host, /lacks ICoreWebView2_18/);
  assert.match(host, /lacks ICoreWebView2Settings8/);
});

run('no Agent Browser security mode, badge or certificate exception UI exists', () => {
  const settings = source('web/src/components/SettingsPage.jsx');
  const panel = source('web/src/components/AgentBrowserPanel.jsx');
  const surface = source('web/src/lib/agentBrowserSurface.js');
  const styles = source('web/src/styles/globals.css');

  // 用户不应感知这套策略:没有开关、没有徽标、没有确认页、没有逐站点例外。
  const forbidden = [
    /证书验证已关闭/,
    /安全模式/,
    /严格模式/,
    /宽松模式/,
    /开发模式/,
    /证书例外/,
    /insecure/i,
    /allowInsecure/i,
    /ignoreCertificate/i,
    /securityMode/i,
    /permissiveMode/i,
  ];
  for (const pattern of forbidden) {
    assert.doesNotMatch(settings, pattern);
    assert.doesNotMatch(panel, pattern);
    assert.doesNotMatch(surface, pattern);
  }
  assert.doesNotMatch(styles, /ace-agent-browser-security/);
  // 失败页也不能建议「开启安全模式」或「添加证书例外」。
  assert.doesNotMatch(surface, /继续访问|信任此证书|添加例外/);
});

run('permissive browsing stays inside the Agent Browser native boundary', () => {
  const host = source('src/desktop/agent_browser_host.cpp');
  const macHost = source('src/desktop/agent_browser_host_mac.mm');

  // 服务器返回的 401/403 正文与登录重定向是真实网页,不能被折算成传输失败。
  // 两端都只从引擎的导航结果判定成败,不看 HTTP 状态码。
  assert.match(host, /args->get_IsSuccess\(&success\)/);
  assert.doesNotMatch(host, /get_StatusCode/);
  assert.doesNotMatch(macHost, /navigation_failure_kind\([^)]*status_code/);

  // 宽松策略不得注入任何 ACECode 能力面:网页拿不到 bridge、daemon token 或
  // Provider 凭据。
  for (const forbidden of [
    /aceDesktop_/,
    /AddHostObjectToScript/,
    /add_WebMessageReceived/,
  ]) {
    assert.doesNotMatch(host, forbidden);
    assert.doesNotMatch(macHost, forbidden);
  }

  // 放宽只发生在这两个原生 Browser 宿主里。任何一处泄漏到共享 HTTP/TLS 栈、
  // Provider、MCP、daemon 或升级器,都会让「只影响 Agent Browser」的合同失效。
  const allowed = new Set([
    'src/desktop/agent_browser_host.cpp',
    'src/desktop/agent_browser_host_mac.mm',
  ]);
  const permissiveMarkers = [
    /credentialForTrust/,
    /NSURLSessionAuthChallengeUseCredential/,
    /shouldAllowDeprecatedTLS/,
    /SERVER_CERTIFICATE_ERROR_ACTION_ALWAYS_ALLOW/,
    /put_IsReputationCheckingRequired/,
    /fraudulentWebsiteWarningEnabled/,
  ];
  const walk = (dir) => {
    const out = [];
    for (const entry of fs.readdirSync(path.join(repoRoot, dir), { withFileTypes: true })) {
      const relative = `${dir}/${entry.name}`;
      if (entry.isDirectory()) out.push(...walk(relative));
      else if (/\.(cpp|hpp|mm|h|c)$/.test(entry.name)) out.push(relative);
    }
    return out;
  };
  for (const file of walk('src')) {
    if (allowed.has(file)) continue;
    const text = source(file);
    for (const marker of permissiveMarkers) {
      assert.doesNotMatch(text, marker, `${file} leaks permissive policy`);
    }
  }
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
  const goalStatus = source('web/src/components/GoalStatusBar.jsx');
  const queueCards = source('web/src/components/QueueCardList.jsx');
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
  assert.match(goalStatus, /function GoalEditModal[\s\S]*<Modal/);
  assert.match(queueCards, /function QueueCardEditDialog[\s\S]*<Modal/);
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

run('explicit overlay notifications submit native layout before the next frame', () => {
  const panel = source('web/src/components/AgentBrowserPanel.jsx');
  const handlerStart = panel.indexOf('const onNativeSurfaceOverlayChange =');
  const handlerEnd = panel.indexOf('const visualViewport =', handlerStart);
  assert.ok(handlerStart >= 0 && handlerEnd > handlerStart);
  const handler = panel.slice(handlerStart, handlerEnd);

  assert.match(handler, /syncNativeSurface\(\)/);
  assert.match(handler, /scheduleLayout\(\)/);
  assert.ok(handler.indexOf('syncNativeSurface()') < handler.indexOf('scheduleLayout()'));
  assert.match(
    panel,
    /addEventListener\(NATIVE_SURFACE_OVERLAY_EVENT, onNativeSurfaceOverlayChange\)/,
  );
  assert.match(
    panel,
    /removeEventListener\(NATIVE_SURFACE_OVERLAY_EVENT, onNativeSurfaceOverlayChange\)/,
  );
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
