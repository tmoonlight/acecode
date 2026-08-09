export const AGENT_BROWSER_STATE_EVENT = 'acecode:agent-browser-state';

export const AGENT_BROWSER_TOOL_NAMES = new Set([
  'browser_open',
  'browser_navigate',
  'browser_read_page',
  'browser_click',
  'browser_fill',
  'browser_type',
  'browser_press',
  'browser_hover',
  'browser_drag',
  'browser_scroll',
  'browser_wait',
  'browser_screenshot',
  'browser_handle_dialog',
  'browser_evaluate',
  'browser_close',
]);

export function parseAgentBrowserBridgeResult(raw) {
  if (raw == null) return {};
  if (typeof raw === 'object') return raw;
  try {
    const parsed = JSON.parse(String(raw));
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch {
    return {};
  }
}

export function hasNativeAgentBrowser(win = globalThis.window) {
  return win?.__ACECODE_DESKTOP_SHELL__ === true
    && (win?.__ACECODE_OS__ === 'windows' || win?.__ACECODE_OS__ === 'macos')
    && win?.__ACECODE_AGENT_BROWSER_SUPPORTED__ !== false
    && typeof win.aceDesktop_agentBrowserGetState === 'function'
    && typeof win.aceDesktop_agentBrowserSetLayout === 'function'
    && typeof win.aceDesktop_agentBrowserCreatePage === 'function';
}

function localPathToFileUrl(value) {
  const normalized = value.replaceAll('\\', '/');
  const encodedPath = encodeURIComponent(normalized)
    .replaceAll('%2F', '/')
    .replaceAll('%3A', ':');
  const candidate = normalized.startsWith('//')
    ? `file:${encodedPath}`
    : (/^[a-z]:\//i.test(normalized) ? `file:///${encodedPath}` : `file://${encodedPath}`);
  try {
    return new URL(candidate).href;
  } catch {
    return '';
  }
}

function normalizeFileAddress(value) {
  if (/^file:/i.test(value)) {
    try {
      return new URL(value.replaceAll('\\', '/')).href;
    } catch {
      return '';
    }
  }
  if (value.startsWith('/') || /^[a-z]:[\\/]/i.test(value) || /^\\\\/.test(value)) {
    return localPathToFileUrl(value);
  }
  return '';
}

export function normalizeAgentBrowserAddress(input = '') {
  const value = String(input || '').trim();
  if (!value) return '';
  const lower = value.toLowerCase();
  if (lower === 'about:blank' || lower.startsWith('http://') || lower.startsWith('https://')) {
    return value;
  }
  const fileAddress = normalizeFileAddress(value);
  if (fileAddress) return fileAddress;
  if (lower.includes('://') || /^(?:javascript|data|edge|devtools):/.test(lower)) {
    return '';
  }
  if (/\s/.test(value)) {
    return `https://www.bing.com/search?q=${encodeURIComponent(value)}`;
  }
  return `https://${value}`;
}

export function agentBrowserLayoutFromRect(rect, devicePixelRatio = 1, visible = true) {
  const scale = Number.isFinite(Number(devicePixelRatio)) && Number(devicePixelRatio) > 0
    ? Number(devicePixelRatio)
    : 1;
  return {
    x: Math.max(0, Math.round((Number(rect?.left) || 0) * scale)),
    y: Math.max(0, Math.round((Number(rect?.top) || 0) * scale)),
    width: Math.max(0, Math.round((Number(rect?.width) || 0) * scale)),
    height: Math.max(0, Math.round((Number(rect?.height) || 0) * scale)),
    visible: !!visible,
  };
}

export function agentBrowserActivityFromItems(items = []) {
  let activationKey = '';
  let pageId = '';
  let toolName = '';
  const liveIds = new Set();
  const liveActivationKeys = [];
  for (const item of Array.isArray(items) ? items : []) {
    if (item?.kind !== 'tool' || !AGENT_BROWSER_TOOL_NAMES.has(item.tool?.tool)) continue;
    const id = String(item.id ?? item.tool?.id ?? item.tool?.toolCallId ?? '');
    activationKey = id || `${item.tool.tool}:${activationKey}`;
    if (!item.tool?.isDone) {
      const liveKey = id || item.tool.tool;
      liveIds.add(liveKey);
      liveActivationKeys.push(liveKey);
      toolName = item.tool.tool;
      pageId = String(item.tool?.args?.page_id || item.tool?.metadata?.page_id || pageId || '');
    }
  }
  return {
    activationKey: liveActivationKeys.length > 0
      ? liveActivationKeys.join('|')
      : activationKey,
    active: liveIds.size > 0,
    liveCount: liveIds.size,
    pageId,
    toolName,
  };
}

export async function getAgentBrowserState(pageId = '', win = globalThis.window) {
  if (!hasNativeAgentBrowser(win)) {
    return { ok: false, supported: false, error: '仅 ACECode Desktop 支持内嵌浏览器' };
  }
  return parseAgentBrowserBridgeResult(
    pageId
      ? await win.aceDesktop_agentBrowserGetState(pageId)
      : await win.aceDesktop_agentBrowserGetState(),
  );
}

export async function createAgentBrowserPage(win = globalThis.window) {
  return runAgentBrowserBridgeAction('aceDesktop_agentBrowserCreatePage', undefined, win);
}

export async function selectAgentBrowserPage(pageId, win = globalThis.window) {
  return runAgentBrowserBridgeAction('aceDesktop_agentBrowserSelectPage', pageId, win);
}

export async function closeAgentBrowserPage(pageId, win = globalThis.window) {
  return runAgentBrowserBridgeAction('aceDesktop_agentBrowserClosePage', pageId, win);
}

export async function setAgentBrowserShared(pageId, shared, win = globalThis.window) {
  return runAgentBrowserBridgeAction('aceDesktop_agentBrowserSetShared', {
    page_id: pageId,
    shared: !!shared,
  }, win);
}

export async function toggleAgentBrowserElementSelection(pageId, win = globalThis.window) {
  return runAgentBrowserBridgeAction(
    'aceDesktop_agentBrowserToggleElementSelection', pageId, win,
  );
}

export async function getAgentBrowserConsoleLogs(pageId, win = globalThis.window) {
  return runAgentBrowserBridgeAction(
    'aceDesktop_agentBrowserGetConsoleLogs', pageId, win,
  );
}

export async function toggleAgentBrowserDevTools(pageId, win = globalThis.window) {
  return runAgentBrowserBridgeAction(
    'aceDesktop_agentBrowserToggleDevTools', pageId, win,
  );
}

export async function setAgentBrowserLayout(pageId, layout, win = globalThis.window) {
  if (!hasNativeAgentBrowser(win)) return { ok: false, supported: false };
  const result = parseAgentBrowserBridgeResult(await win.aceDesktop_agentBrowserSetLayout({
    ...layout,
    page_id: pageId,
  }));
  if (result.ok !== true) return result;
  const expectedRevision = Number(layout?.layout_revision) || 0;
  const expectedOcclusionCount = Array.isArray(layout?.occlusion_rects)
    ? layout.occlusion_rects.length
    : 0;
  if (Number(result.layout_revision) !== expectedRevision
      || Number(result.occlusion_rect_count) !== expectedOcclusionCount) {
    return {
      ok: false,
      error: 'Agent Browser layout acknowledgement did not match the request',
    };
  }
  return result;
}

export async function runAgentBrowserBridgeAction(name, value, win = globalThis.window) {
  const fn = win?.[name];
  if (typeof fn !== 'function') {
    return { ok: false, error: 'Agent Browser 桌面桥不可用' };
  }
  const raw = value === undefined ? await fn() : await fn(value);
  return parseAgentBrowserBridgeResult(raw);
}
