import assert from 'node:assert/strict';
import {
  agentBrowserActivityFromItems,
  agentBrowserErrorPresentation,
  agentBrowserLayoutFromRect,
  createAgentBrowserPage,
  hasNativeAgentBrowser,
  normalizeAgentBrowserAddress,
  parseAgentBrowserBridgeResult,
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

await run('Agent Browser presents retryable WebView2 navigation failures in plain language', () => {
  assert.deepEqual(
    agentBrowserErrorPresentation('navigation failed (WebView2 status 13)'),
    {
      message: '无法解析主机名，请检查地址、网络或重试（WebView2 状态 13）',
      retryable: true,
    },
  );
  assert.deepEqual(agentBrowserErrorPresentation('browser URL is empty'), {
    message: 'browser URL is empty',
    retryable: false,
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
