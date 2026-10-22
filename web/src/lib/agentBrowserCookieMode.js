// Agent Browser「保留 Cookie」三档与清除浏览数据(openspec add-agent-browser-cookie-modes)。
//
// 档位由 Desktop 持有(写在 state.json,决定页面用哪份 WebView2 Profile / WebKit 存储),
// 这里只做:三档的文案、清除确认框的文案、一个工具栏菜单与设置页共用的单写者 store,
// 以及 bridge 封装。非 Desktop 环境(普通浏览器)没有这些 bridge,界面不显示任何
// cookie 控件。

import { createSingleWriterStore } from './singleWriterStore.js';
import { parseAgentBrowserBridgeResult } from './agentBrowser.js';

export const AGENT_BROWSER_STORAGE_EVENT = 'acecode:agent-browser-storage';
export const DEFAULT_AGENT_BROWSER_COOKIE_MODE = 'shared';

export const AGENT_BROWSER_COOKIE_MODES = Object.freeze([
  Object.freeze({
    value: 'until_quit',
    label: '退出时清除',
    hint: '退出 ACECode 时清除 Cookie',
  }),
  Object.freeze({
    value: 'shared',
    label: '共享',
    hint: '所有会话共用同一份 Cookie',
  }),
  Object.freeze({
    value: 'per_session',
    label: '按会话',
    hint: '每个会话各自保存 Cookie',
  }),
]);

const MODE_VALUES = new Set(AGENT_BROWSER_COOKIE_MODES.map((option) => option.value));

export function isAgentBrowserCookieMode(value) {
  return MODE_VALUES.has(value);
}

// Desktop 回来的档位名 → 界面档位;空与未知值按共享(与 native 的默认一致)。
export function normalizeAgentBrowserCookieMode(value) {
  return isAgentBrowserCookieMode(value) ? value : DEFAULT_AGENT_BROWSER_COOKIE_MODE;
}

export function agentBrowserCookieModeOption(value) {
  const mode = normalizeAgentBrowserCookieMode(value);
  return AGENT_BROWSER_COOKIE_MODES.find((option) => option.value === mode);
}

// 「清除浏览数据」确认框:清除的是当前页面所用的那份存储,范围随档位变化,
// 文案必须说清楚会影响谁。
export function agentBrowserClearBrowsingDataDialog(mode) {
  const normalized = normalizeAgentBrowserCookieMode(mode);
  const common = {
    title: '清除浏览数据?',
    confirmLabel: '清除',
    cancelLabel: '取消',
  };
  if (normalized === 'per_session') {
    return {
      ...common,
      scope: 'session',
      body: '将删除当前会话浏览器里保存的 Cookie、登录状态和网站数据,其它会话不受影响。之后需要重新登录网站。',
    };
  }
  if (normalized === 'until_quit') {
    return {
      ...common,
      scope: 'until_quit',
      body: '将删除本次运行中保存的 Cookie、登录状态和网站数据(退出 ACECode 时本来也会清除)。之后需要重新登录网站。',
    };
  }
  return {
    ...common,
    scope: 'shared',
    body: '将删除所有会话共用的 Cookie、登录状态和网站数据。之后需要重新登录网站。',
  };
}

export function hasAgentBrowserCookieBridge(win = globalThis.window) {
  return typeof win?.aceDesktop_agentBrowserGetCookieMode === 'function'
    && typeof win?.aceDesktop_agentBrowserSetCookieMode === 'function'
    && typeof win?.aceDesktop_agentBrowserClearBrowsingData === 'function';
}

const INITIAL_STATE = Object.freeze({
  supported: false,
  loaded: false,
  mode: DEFAULT_AGENT_BROWSER_COOKIE_MODE,
  // 正在切换到的档位(bridge 还没返回);空 = 没有在途的切换。
  pending: '',
  error: '',
});

export function createAgentBrowserCookieModeStore() {
  return createSingleWriterStore(INITIAL_STATE);
}

// 工具栏菜单与设置页共用这一份。
export const agentBrowserCookieModeStore = createAgentBrowserCookieModeStore();

function sameState(a, b) {
  return a.supported === b.supported && a.loaded === b.loaded && a.mode === b.mode
    && a.pending === b.pending && a.error === b.error;
}

function commitPatch(store, patch) {
  return store.commit((state) => {
    const next = { ...state, ...patch };
    return sameState(state, next) ? state : next;
  });
}

export async function loadAgentBrowserCookieMode(
  win = globalThis.window,
  store = agentBrowserCookieModeStore,
) {
  if (!hasAgentBrowserCookieBridge(win)) {
    return commitPatch(store, { supported: false, loaded: true });
  }
  let result;
  try {
    result = parseAgentBrowserBridgeResult(await win.aceDesktop_agentBrowserGetCookieMode());
  } catch (error) {
    result = { ok: false, error: String(error?.message || error || '') };
  }
  if (result.ok !== true) {
    return commitPatch(store, {
      loaded: true,
      error: result.error || '无法读取 Cookie 设置',
    });
  }
  return commitPatch(store, {
    supported: result.supported !== false,
    loaded: true,
    mode: normalizeAgentBrowserCookieMode(result.mode),
    error: '',
  });
}

// 切档。失败时档位保持原值、error 带原因;成功以 Desktop 返回的档位为准。
export async function setAgentBrowserCookieMode(
  mode,
  win = globalThis.window,
  store = agentBrowserCookieModeStore,
) {
  if (!isAgentBrowserCookieMode(mode)) {
    return { ok: false, error: `未知的 Cookie 档位:${mode}` };
  }
  if (!hasAgentBrowserCookieBridge(win)) {
    return { ok: false, error: 'Agent Browser 桌面桥不可用' };
  }
  commitPatch(store, { pending: mode, error: '' });
  let result;
  try {
    result = parseAgentBrowserBridgeResult(await win.aceDesktop_agentBrowserSetCookieMode(mode));
  } catch (error) {
    result = { ok: false, error: String(error?.message || error || '') };
  }
  if (result.ok === true) {
    commitPatch(store, {
      mode: normalizeAgentBrowserCookieMode(result.mode || mode),
      pending: '',
      error: '',
    });
  } else {
    commitPatch(store, { pending: '', error: result.error || '无法保存 Cookie 设置' });
  }
  return result;
}

// 只发起清除;完成由 AGENT_BROWSER_STORAGE_EVENT(type=browsing_data_cleared)回报。
export async function clearAgentBrowserBrowsingData(pageId, win = globalThis.window) {
  const fn = win?.aceDesktop_agentBrowserClearBrowsingData;
  if (typeof fn !== 'function') return { ok: false, error: 'Agent Browser 桌面桥不可用' };
  try {
    return parseAgentBrowserBridgeResult(await fn(pageId));
  } catch (error) {
    return { ok: false, error: String(error?.message || error || '') };
  }
}

// 等一条满足条件的存储事件;超时返回 { ok:false, timedOut:true }。
export function waitForAgentBrowserStorageEvent(
  predicate,
  { timeoutMs = 30000 } = {},
  win = globalThis.window,
) {
  return new Promise((resolve) => {
    if (typeof win?.addEventListener !== 'function') {
      resolve({ ok: false, error: 'Agent Browser 桌面桥不可用' });
      return;
    }
    let timer = null;
    const onEvent = (event) => {
      const detail = event?.detail;
      if (!detail || typeof detail !== 'object' || !predicate(detail)) return;
      cleanup();
      resolve(detail);
    };
    const cleanup = () => {
      win.removeEventListener(AGENT_BROWSER_STORAGE_EVENT, onEvent);
      if (timer) clearTimeout(timer);
    };
    win.addEventListener(AGENT_BROWSER_STORAGE_EVENT, onEvent);
    timer = setTimeout(() => {
      cleanup();
      resolve({ ok: false, timedOut: true, error: '清除浏览数据超时' });
    }, timeoutMs);
  });
}

// Desktop 侧档位变化(切档成功事件)同步进 store;返回注销函数。
export function installAgentBrowserCookieModeListener(
  win = globalThis.window,
  store = agentBrowserCookieModeStore,
) {
  if (typeof win?.addEventListener !== 'function') return () => {};
  const onEvent = (event) => {
    const detail = event?.detail;
    if (!detail || detail.type !== 'cookie_mode_changed' || detail.ok === false) return;
    if (!isAgentBrowserCookieMode(detail.cookie_mode)) return;
    commitPatch(store, { mode: detail.cookie_mode });
  };
  win.addEventListener(AGENT_BROWSER_STORAGE_EVENT, onEvent);
  return () => win.removeEventListener(AGENT_BROWSER_STORAGE_EVENT, onEvent);
}
