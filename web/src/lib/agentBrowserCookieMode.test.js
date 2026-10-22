import assert from 'node:assert/strict';
import {
  AGENT_BROWSER_COOKIE_MODES,
  AGENT_BROWSER_STORAGE_EVENT,
  agentBrowserClearBrowsingDataDialog,
  agentBrowserCookieModeOption,
  clearAgentBrowserBrowsingData,
  createAgentBrowserCookieModeStore,
  hasAgentBrowserCookieBridge,
  installAgentBrowserCookieModeListener,
  loadAgentBrowserCookieMode,
  normalizeAgentBrowserCookieMode,
  setAgentBrowserCookieMode,
  waitForAgentBrowserStorageEvent,
} from './agentBrowserCookieMode.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 最小的 window 替身:三个 cookie bridge + 事件派发。
function fakeDesktop({ mode = 'shared', setResult, throwOnSet = false } = {}) {
  const listeners = new Map();
  const calls = [];
  return {
    calls,
    aceDesktop_agentBrowserGetCookieMode() {
      calls.push(['get']);
      return JSON.stringify({ ok: true, supported: true, mode });
    },
    aceDesktop_agentBrowserSetCookieMode(next) {
      calls.push(['set', next]);
      if (throwOnSet) throw new Error('bridge crashed');
      return JSON.stringify(setResult ?? { ok: true, mode: next });
    },
    aceDesktop_agentBrowserClearBrowsingData(pageId) {
      calls.push(['clear', pageId]);
      return JSON.stringify({ ok: true, started: true, page_id: pageId });
    },
    addEventListener(type, fn) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(fn);
    },
    removeEventListener(type, fn) {
      listeners.get(type)?.delete(fn);
    },
    dispatch(detail) {
      for (const fn of [...(listeners.get(AGENT_BROWSER_STORAGE_EVENT) || [])]) fn({ detail });
    },
    listenerCount() {
      return listeners.get(AGENT_BROWSER_STORAGE_EVENT)?.size || 0;
    },
  };
}

// 场景:三档与 Claude 的 Until quit / Shared / Per session 一一对应,默认共享。
// 期望:未知 / 空的档位名一律按共享显示,与 native 的默认一致。
await run('cookie modes map unknown values to the shared default', () => {
  assert.deepEqual(AGENT_BROWSER_COOKIE_MODES.map((option) => option.value),
    ['until_quit', 'shared', 'per_session']);
  assert.equal(normalizeAgentBrowserCookieMode('per_session'), 'per_session');
  assert.equal(normalizeAgentBrowserCookieMode(''), 'shared');
  assert.equal(normalizeAgentBrowserCookieMode('per_workspace'), 'shared');
  assert.equal(agentBrowserCookieModeOption('until_quit').label, '退出时清除');
  for (const option of AGENT_BROWSER_COOKIE_MODES) {
    assert.ok(option.label && option.hint, option.value);
  }
});

// 场景:用户点「清除浏览数据」,确认框必须说清楚这次会清掉谁的数据。
// 期望:共享档说「所有会话」;按会话档说「当前会话」且其它会话不受影响;退出即清档说「本次运行」。
await run('clear browsing data dialog states the scope of each mode', () => {
  const shared = agentBrowserClearBrowsingDataDialog('shared');
  assert.equal(shared.scope, 'shared');
  assert.match(shared.body, /所有会话/);
  const session = agentBrowserClearBrowsingDataDialog('per_session');
  assert.equal(session.scope, 'session');
  assert.match(session.body, /当前会话/);
  assert.match(session.body, /其它会话不受影响/);
  const ephemeral = agentBrowserClearBrowsingDataDialog('until_quit');
  assert.equal(ephemeral.scope, 'until_quit');
  assert.match(ephemeral.body, /本次运行/);
  assert.equal(shared.confirmLabel, '清除');
  assert.equal(agentBrowserClearBrowsingDataDialog('bogus').scope, 'shared');
});

// 场景:Web UI 在普通浏览器里打开(没有 Desktop bridge)。
// 期望:标记为不支持(界面据此不显示 cookie 控件),切档直接失败且不抛异常。
await run('without the Desktop bridge cookie controls are unsupported', async () => {
  const store = createAgentBrowserCookieModeStore();
  const state = await loadAgentBrowserCookieMode({}, store);
  assert.equal(state.supported, false);
  assert.equal(state.loaded, true);
  assert.equal(hasAgentBrowserCookieBridge({}), false);
  const result = await setAgentBrowserCookieMode('per_session', {}, store);
  assert.equal(result.ok, false);
  assert.equal((await clearAgentBrowserBrowsingData('p1', {})).ok, false);
});

// 场景:Desktop 里打开,之前选过「按会话」。
// 期望:读到的档位进入共享 store,工具栏与设置页看到的是同一个值。
await run('loading reads the persisted mode from Desktop', async () => {
  const win = fakeDesktop({ mode: 'per_session' });
  const store = createAgentBrowserCookieModeStore();
  await loadAgentBrowserCookieMode(win, store);
  assert.deepEqual(store.getState(), {
    supported: true, loaded: true, mode: 'per_session', pending: '', error: '',
  });
});

// 场景:用户在菜单里改档位,Desktop 接受。
// 期望:bridge 收到规范名;切换期间 pending 指向新档位,完成后 mode 更新、pending 清空。
await run('setting a mode updates the shared store on success', async () => {
  const win = fakeDesktop();
  const store = createAgentBrowserCookieModeStore();
  await loadAgentBrowserCookieMode(win, store);
  const seen = [];
  store.subscribe(() => seen.push(store.getState().pending));
  const result = await setAgentBrowserCookieMode('until_quit', win, store);
  assert.equal(result.ok, true);
  assert.deepEqual(win.calls.at(-1), ['set', 'until_quit']);
  assert.equal(store.getState().mode, 'until_quit');
  assert.equal(store.getState().pending, '');
  assert.ok(seen.includes('until_quit'), 'pending was published while switching');
});

// 场景:Desktop 拒绝(状态文件写不进去)或 bridge 直接抛异常。
// 期望:档位保持原值,error 带原因 —— 界面不能显示一个其实没生效的档位。
await run('a rejected or crashing switch keeps the previous mode', async () => {
  const rejecting = fakeDesktop({ setResult: { ok: false, error: 'disk full' } });
  const store = createAgentBrowserCookieModeStore();
  await loadAgentBrowserCookieMode(rejecting, store);
  const rejected = await setAgentBrowserCookieMode('per_session', rejecting, store);
  assert.equal(rejected.ok, false);
  assert.equal(store.getState().mode, 'shared');
  assert.equal(store.getState().error, 'disk full');

  const crashing = fakeDesktop({ throwOnSet: true });
  const crashed = await setAgentBrowserCookieMode('per_session', crashing, store);
  assert.equal(crashed.ok, false);
  assert.equal(store.getState().mode, 'shared');
  assert.match(store.getState().error, /bridge crashed/);
  assert.equal((await setAgentBrowserCookieMode('bogus', crashing, store)).ok, false);
});

// 场景:清除浏览数据是异步的,bridge 只返回「已开始」,结果靠存储事件回报。
// 期望:等待只认匹配的事件(别的页面 / 别的类型不算),等到后注销监听;超时有明确结果。
await run('waiting for the clear result matches the right storage event', async () => {
  const win = fakeDesktop();
  const started = await clearAgentBrowserBrowsingData('page-7', win);
  assert.equal(started.started, true);
  const waiting = waitForAgentBrowserStorageEvent(
    (detail) => detail.type === 'browsing_data_cleared' && detail.page_id === 'page-7',
    { timeoutMs: 1000 },
    win,
  );
  win.dispatch({ type: 'browsing_data_cleared', page_id: 'other', ok: true });
  win.dispatch({ type: 'cookie_mode_changed', page_id: 'page-7', ok: true });
  win.dispatch({ type: 'browsing_data_cleared', page_id: 'page-7', ok: true, store: 'shared' });
  const detail = await waiting;
  assert.equal(detail.store, 'shared');
  assert.equal(win.listenerCount(), 0);

  const timedOut = await waitForAgentBrowserStorageEvent(() => true, { timeoutMs: 5 }, win);
  assert.equal(timedOut.timedOut, true);
  assert.equal(win.listenerCount(), 0);
});

// 场景:Desktop 侧档位变了(切档成功事件)。
// 期望:合法档位同步进 store;失败事件与未知档位名忽略;注销后不再更新。
await run('cookie mode events from Desktop update the store', async () => {
  const win = fakeDesktop();
  const store = createAgentBrowserCookieModeStore();
  const uninstall = installAgentBrowserCookieModeListener(win, store);
  win.dispatch({ type: 'cookie_mode_changed', ok: true, cookie_mode: 'per_session' });
  assert.equal(store.getState().mode, 'per_session');
  win.dispatch({ type: 'cookie_mode_changed', ok: false, cookie_mode: 'until_quit' });
  win.dispatch({ type: 'cookie_mode_changed', ok: true, cookie_mode: 'bogus' });
  assert.equal(store.getState().mode, 'per_session');
  uninstall();
  win.dispatch({ type: 'cookie_mode_changed', ok: true, cookie_mode: 'until_quit' });
  assert.equal(store.getState().mode, 'per_session');
});
