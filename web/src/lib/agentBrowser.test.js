import assert from 'node:assert/strict';
import {
  agentBrowserActivityFromItems,
  agentBrowserLayoutFromRect,
  createAgentBrowserPage,
  getAgentBrowserConsoleLogs,
  hasNativeAgentBrowser,
  normalizeAgentBrowserAddress,
  parseAgentBrowserBridgeResult,
  setAgentBrowserShared,
  toggleAgentBrowserDevTools,
  toggleAgentBrowserElementSelection,
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

await run('Agent Browser address normalization mirrors the native web-only policy', () => {
  assert.equal(normalizeAgentBrowserAddress(' example.com/a '), 'https://example.com/a');
  assert.equal(normalizeAgentBrowserAddress('HTTP://localhost:3000'), 'HTTP://localhost:3000');
  assert.equal(
    normalizeAgentBrowserAddress('webview2 agent browser'),
    'https://www.bing.com/search?q=webview2%20agent%20browser',
  );
  assert.equal(normalizeAgentBrowserAddress('file:///C:/secret.txt'), '');
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
  assert.deepEqual(await createAgentBrowserPage({}), {
    ok: false,
    error: 'Agent Browser 桌面桥不可用',
  });
  assert.deepEqual(await createAgentBrowserPage({
    aceDesktop_agentBrowserCreatePage() {
      return '{"ok":true,"page_id":"browser-2"}';
    },
  }), {
    ok: true,
    page_id: 'browser-2',
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
