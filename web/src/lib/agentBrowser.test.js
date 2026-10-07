import assert from 'node:assert/strict';
import {
  agentBrowserActivityFromItems,
  agentBrowserLayoutFromRect,
  agentBrowserOwnerForSession,
  createAgentBrowserPage,
  getAgentBrowserConsoleLogs,
  hasNativeAgentBrowser,
  listAgentBrowserPages,
  normalizeAgentBrowserAddress,
  parseAgentBrowserBridgeResult,
  setAgentBrowserLayout,
  setAgentBrowserShared,
  toggleAgentBrowserDevTools,
  toggleAgentBrowserElementSelection,
  waitForAgentBrowserPageReady,
} from './agentBrowser.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('Agent Browser waits for native page readiness before immediate navigation', async () => {
  const states = [{ ok: true, ready: false }, { ok: true, ready: true }];
  const calls = [];
  const win = {
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'windows',
    aceDesktop_agentBrowserCreatePage() {},
    aceDesktop_agentBrowserSetLayout() {},
    aceDesktop_agentBrowserGetState(pageId) { calls.push(pageId); return states.shift(); },
  };
  assert.equal((await waitForAgentBrowserPageReady('page-1', { pollMs: 0 }, win)).ready, true);
  assert.deepEqual(calls, ['page-1', 'page-1']);
  win.aceDesktop_agentBrowserGetState = () => ({ ok: true, ready: false });
  assert.match((await waitForAgentBrowserPageReady('page-1', { timeoutMs: 0 }, win)).error, /超时/);
  win.aceDesktop_agentBrowserGetState = () => ({ ok: false, error: 'native failed' });
  assert.equal((await waitForAgentBrowserPageReady('page-1', {}, win)).error, 'native failed');
  win.aceDesktop_agentBrowserGetState = () => ({ ok: true, closed: true });
  assert.match((await waitForAgentBrowserPageReady('page-1', {}, win)).error, /已关闭/);
  let current = true;
  win.aceDesktop_agentBrowserGetState = () => { current = false; return { ok: true, ready: true }; };
  assert.equal((await waitForAgentBrowserPageReady('page-1', { isCurrent: () => current }, win)).cancelled, true);
  win.aceDesktop_agentBrowserGetState = () => { throw new Error('must not read a stale page'); };
  assert.equal((await waitForAgentBrowserPageReady('page-1', { isCurrent: () => false }, win)).cancelled, true);
});

await run('Agent Browser address normalization accepts web and absolute local paths', () => {
  assert.equal(normalizeAgentBrowserAddress(' example.com/a '), 'https://example.com/a');
  assert.equal(normalizeAgentBrowserAddress('HTTP://localhost:3000'), 'HTTP://localhost:3000');
  assert.equal(
    normalizeAgentBrowserAddress('webview2 agent browser'),
    'https://www.bing.com/search?q=webview2%20agent%20browser',
  );
  assert.equal(
    normalizeAgentBrowserAddress('file:///C:/Program Files/page.html'),
    'file:///C:/Program%20Files/page.html',
  );
  assert.equal(
    normalizeAgentBrowserAddress('C:\\Users\\Test User\\page.html'),
    'file:///C:/Users/Test%20User/page.html',
  );
  assert.equal(
    normalizeAgentBrowserAddress('\\\\server\\share\\page one.html'),
    'file://server/share/page%20one.html',
  );
  assert.equal(
    normalizeAgentBrowserAddress('/Users/test/本地 page.html'),
    'file:///Users/test/%E6%9C%AC%E5%9C%B0%20page.html',
  );
  assert.equal(normalizeAgentBrowserAddress('javascript:alert(1)'), '');
});

await run('Agent Browser layout converts CSS viewport coordinates to physical pixels', () => {
  assert.deepEqual(agentBrowserLayoutFromRect({ left: 10.25, top: 20, width: 300.5, height: 200 }, 1.5), {
    x: 15, y: 30, width: 451, height: 300, visible: true,
  });
});

await run('Agent Browser activity retains activation identity and animates only live calls', () => {
  const first = agentBrowserActivityFromItems([
    { kind: 'tool', id: 'one', tool: { tool: 'browser_navigate', isDone: true } },
  ]);
  assert.deepEqual(first, {
    activationKey: 'one', active: false, liveCount: 0, pageId: '', toolName: '',
  });
  const second = agentBrowserActivityFromItems([
    { kind: 'tool', id: 'one', tool: { tool: 'browser_navigate', isDone: true } },
    {
      kind: 'tool',
      id: 'two',
      tool: { tool: 'browser_click', isDone: false, args: { page_id: 'page-2' } },
    },
  ]);
  assert.deepEqual(second, {
    activationKey: 'two', active: true, liveCount: 1, pageId: 'page-2', toolName: 'browser_click',
  });

  const parallel = agentBrowserActivityFromItems([
    {
      kind: 'tool', id: 'one',
      tool: { tool: 'browser_wait', isDone: false, args: { page_id: 'page-1' } },
    },
    {
      kind: 'tool', id: 'two',
      tool: { tool: 'browser_click', isDone: false, args: { page_id: 'page-2' } },
    },
  ]);
  const firstStillLive = agentBrowserActivityFromItems([
    {
      kind: 'tool', id: 'one',
      tool: { tool: 'browser_wait', isDone: false, args: { page_id: 'page-1' } },
    },
    {
      kind: 'tool', id: 'two',
      tool: { tool: 'browser_click', isDone: true, args: { page_id: 'page-2' } },
    },
  ]);
  assert.equal(parallel.activationKey, 'one|two');
  assert.equal(parallel.pageId, 'page-2');
  assert.equal(firstStillLive.activationKey, 'one');
  assert.equal(firstStillLive.pageId, 'page-1');
});

await run('Agent Browser desktop bridge detection and JSON parsing are defensive', () => {
  assert.equal(hasNativeAgentBrowser({
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'windows',
    aceDesktop_agentBrowserGetState() {},
    aceDesktop_agentBrowserSetLayout() {},
    aceDesktop_agentBrowserCreatePage() {},
  }), true);
  assert.equal(hasNativeAgentBrowser({
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'macos',
    aceDesktop_agentBrowserGetState() {},
    aceDesktop_agentBrowserSetLayout() {},
    aceDesktop_agentBrowserCreatePage() {},
  }), true);
  assert.equal(hasNativeAgentBrowser({
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'macos',
    __ACECODE_AGENT_BROWSER_SUPPORTED__: false,
    aceDesktop_agentBrowserGetState() {},
    aceDesktop_agentBrowserSetLayout() {},
    aceDesktop_agentBrowserCreatePage() {},
  }), false);
  assert.equal(hasNativeAgentBrowser({}), false);
  assert.deepEqual(parseAgentBrowserBridgeResult('{"ok":true}'), { ok: true });
  assert.deepEqual(parseAgentBrowserBridgeResult('bad'), {});
});

await run('Agent Browser page creation checks its dedicated desktop bridge', async () => {
  assert.deepEqual(await createAgentBrowserPage(null, {}), {
    ok: false,
    error: 'Agent Browser 桌面桥不可用',
  });
  // 旧签名 createAgentBrowserPage(win) 仍然兼容。
  assert.deepEqual(await createAgentBrowserPage({
    aceDesktop_agentBrowserCreatePage() {
      return '{"ok":true,"page_id":"browser-2"}';
    },
  }), {
    ok: true,
    page_id: 'browser-2',
  });
});

// 场景:UI 自建页与页面列表都把会话归属传给 Desktop。
// 期望:owner 参数原样作为 bridge 的唯一参数;没有 owner 时不传参数(旧 Desktop
// 的 CreatePage 忽略参数,新 Desktop 视为未绑定);ListPages 按 session_id 过滤。
await run('Agent Browser page creation and listing forward the session owner', async () => {
  const calls = [];
  const win = {
    aceDesktop_agentBrowserCreatePage(arg) {
      calls.push(['create', arg]);
      return '{"ok":true,"page_id":"browser-3"}';
    },
    aceDesktop_agentBrowserListPages(arg) {
      calls.push(['list', arg]);
      return '{"ok":true,"pages":[]}';
    },
  };
  assert.deepEqual(agentBrowserOwnerForSession({ sessionId: 's1', workspaceHash: 'w1' }), {
    session_id: 's1', workspace_hash: 'w1',
  });
  assert.deepEqual(agentBrowserOwnerForSession({ id: 's2' }), { session_id: 's2' });
  assert.deepEqual(agentBrowserOwnerForSession('s3'), { session_id: 's3' });
  assert.equal(agentBrowserOwnerForSession(null), null);
  assert.equal(agentBrowserOwnerForSession({ workspaceHash: 'w1' }), null);

  await createAgentBrowserPage({ session_id: 's1', workspace_hash: 'w1' }, win);
  await createAgentBrowserPage(null, win);
  await listAgentBrowserPages('s1', win);
  await listAgentBrowserPages('', win);
  assert.deepEqual(calls, [
    ['create', { session_id: 's1', workspace_hash: 'w1' }],
    ['create', undefined],
    ['list', { session_id: 's1' }],
    ['list', undefined],
  ]);
  assert.deepEqual(await listAgentBrowserPages('s1', {}), {
    ok: false,
    error: 'Agent Browser 桌面桥不可用',
  });
});

await run('Agent Browser layout requires a matching native acknowledgement', async () => {
  const layout = {
    x: 10,
    y: 20,
    width: 300,
    height: 200,
    visible: true,
    layout_revision: 77,
    occlusion_rects: [{ x: 20, y: 30, width: 80, height: 60 }],
  };
  const desktop = {
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'macos',
    aceDesktop_agentBrowserGetState() {},
    aceDesktop_agentBrowserCreatePage() {},
    aceDesktop_agentBrowserSetLayout() {
      return JSON.stringify({
        ok: true,
        layout_revision: 77,
        occlusion_rect_count: 1,
      });
    },
  };
  assert.equal((await setAgentBrowserLayout('page-1', layout, desktop)).ok, true);
  desktop.aceDesktop_agentBrowserSetLayout = () => JSON.stringify({
    ok: true,
    layout_revision: 76,
    occlusion_rect_count: 1,
  });
  assert.deepEqual(await setAgentBrowserLayout('page-1', layout, desktop), {
    ok: false,
    error: 'Agent Browser layout acknowledgement did not match the request',
  });
});

await run('Agent Browser collaboration actions target the exact page bridge', async () => {
  const calls = [];
  const win = {
    aceDesktop_agentBrowserSetShared(value) {
      calls.push(['share', value]);
      return JSON.stringify({ ok: true, shared_with_agent: value.shared });
    },
    aceDesktop_agentBrowserToggleElementSelection(value) {
      calls.push(['element', value]);
      return '{"ok":true}';
    },
    aceDesktop_agentBrowserGetConsoleLogs(value) {
      calls.push(['console', value]);
      return '{"ok":true,"logs":"[log] ready"}';
    },
    aceDesktop_agentBrowserToggleDevTools(value) {
      calls.push(['devtools', value]);
      return '{"ok":true}';
    },
  };
  await setAgentBrowserShared('page-2', true, win);
  await toggleAgentBrowserElementSelection('page-2', win);
  assert.equal((await getAgentBrowserConsoleLogs('page-2', win)).logs, '[log] ready');
  await toggleAgentBrowserDevTools('page-2', win);
  assert.deepEqual(calls, [
    ['share', { page_id: 'page-2', shared: true }],
    ['element', 'page-2'],
    ['console', 'page-2'],
    ['devtools', 'page-2'],
  ]);
});
