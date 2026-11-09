import assert from 'node:assert/strict';
import {
  clipboardPasteButtonAvailable,
  copyImageToSystemClipboard,
  copyTextToSystemClipboard,
  readTextFromSystemClipboard,
} from './systemClipboard.js';

function run(name, fn) {
  try {
    const ret = fn();
    if (ret && typeof ret.then === 'function') {
      return ret.then(
        () => console.log(`[pass] ${name}`),
        (err) => { console.error(`[fail] ${name}`); throw err; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('system clipboard can explicitly copy an empty file without changing default empty-input behavior', async () => {
  const calls = [];
  const win = { aceDesktop_writeClipboardText: async text => { calls.push(text); return { ok: true }; } };
  assert.equal((await copyTextToSystemClipboard('', win)).ok, false);
  assert.deepEqual(calls, []);
  assert.equal((await copyTextToSystemClipboard('', win, { allowEmpty: true })).ok, true);
  assert.deepEqual(calls, ['']);
});

await run('system clipboard uses desktop bridge first', async () => {
  const calls = [];
  const result = await copyTextToSystemClipboard('1BEA-E219', {
    aceDesktop_writeClipboardText: async (text) => {
      calls.push(text);
      return JSON.stringify({ ok: true });
    },
    navigator: {
      clipboard: {
        writeText: () => { throw new Error('navigator clipboard must not be used'); },
      },
    },
  });

  assert.deepEqual(calls, ['1BEA-E219']);
  assert.deepEqual(result, { ok: true, via: 'desktop' });
});

await run('system clipboard falls back to navigator outside desktop shell', async () => {
  const calls = [];
  const result = await copyTextToSystemClipboard('1BEA-E219', {
    navigator: {
      clipboard: {
        writeText: async (text) => { calls.push(text); },
      },
    },
  });

  assert.deepEqual(calls, ['1BEA-E219']);
  assert.deepEqual(result, { ok: true, via: 'navigator' });
});

await run('system clipboard reports desktop bridge failure when no fallback exists', async () => {
  const result = await copyTextToSystemClipboard('1BEA-E219', {
    aceDesktop_writeClipboardText: async () => JSON.stringify({ ok: false, error: 'native failed' }),
  });

  assert.equal(result.ok, false);
  assert.equal(result.error, 'native failed');
});

await run('image clipboard writes fetched image blob as ClipboardItem', async () => {
  const writes = [];
  const items = [];
  class FakeClipboardItem {
    constructor(payload) {
      items.push(payload);
      this.payload = payload;
    }
  }

  const result = await copyImageToSystemClipboard('/api/blob', {
    mimeType: 'image/png',
    fetchImpl: async (url) => ({
      ok: true,
      status: 200,
      blob: async () => new Blob(['png'], { type: 'image/png' }),
      url,
    }),
    win: {
      ClipboardItem: FakeClipboardItem,
      navigator: {
        clipboard: {
          write: async (payload) => { writes.push(payload); },
        },
      },
    },
  });

  assert.equal(result.ok, true);
  assert.equal(result.mimeType, 'image/png');
  assert.equal(writes.length, 1);
  assert.equal(writes[0][0] instanceof FakeClipboardItem, true);
  assert.equal(items[0]['image/png'].type, 'image/png');
});

await run('image clipboard converts non-png images when direct write is unsupported', async () => {
  const writes = [];
  const items = [];
  const draws = [];
  const closes = [];
  const convertedBlob = new Blob(['png'], { type: 'image/png' });
  class FakeClipboardItem {
    constructor(payload) {
      items.push(payload);
      this.payload = payload;
    }
  }

  const result = await copyImageToSystemClipboard('/api/photo', {
    mimeType: 'image/jpeg',
    fetchImpl: async () => ({
      ok: true,
      status: 200,
      blob: async () => new Blob(['jpeg'], { type: 'image/jpeg' }),
    }),
    win: {
      ClipboardItem: FakeClipboardItem,
      createImageBitmap: async () => ({
        width: 8,
        height: 6,
        close: () => { closes.push(true); },
      }),
      document: {
        createElement: () => ({
          width: 0,
          height: 0,
          getContext: () => ({
            drawImage: (...args) => { draws.push(args); },
          }),
          toBlob: (callback, type) => {
            assert.equal(type, 'image/png');
            callback(convertedBlob);
          },
        }),
      },
      navigator: {
        clipboard: {
          write: async (payload) => {
            writes.push(payload);
            if (writes.length === 1) throw new Error('image/jpeg unsupported');
          },
        },
      },
    },
  });

  assert.equal(result.ok, true);
  assert.equal(result.mimeType, 'image/png');
  assert.equal(result.converted, true);
  assert.equal(writes.length, 2);
  assert.equal(items[0]['image/jpeg'].type, 'image/jpeg');
  assert.equal(items[1]['image/png'].type, 'image/png');
  assert.equal(draws.length, 1);
  assert.deepEqual(closes, [true]);
});

await run('image clipboard reports unavailable binary clipboard support', async () => {
  const result = await copyImageToSystemClipboard('/api/blob', {
    win: {
      navigator: {
        clipboard: {
          writeText: async () => {},
        },
      },
    },
  });

  assert.equal(result.ok, false);
  assert.equal(result.error, 'image clipboard unavailable');
});

await run('image clipboard rejects non-image blobs', async () => {
  const result = await copyImageToSystemClipboard('/api/blob', {
    fetchImpl: async () => ({
      ok: true,
      blob: async () => new Blob(['text'], { type: 'text/plain' }),
    }),
    win: {
      ClipboardItem: class {},
      navigator: {
        clipboard: {
          write: async () => {},
        },
      },
    },
  });

  assert.equal(result.ok, false);
  assert.equal(result.error, 'clipboard item is not an image');
});

// 安卓 App 注入的 aceAndroid 通道的假实现:收到读剪贴板请求后按同一个 id 回包。
function fakeAndroidBridge(reply) {
  const listeners = new Set();
  const sent = [];
  return {
    sent,
    postMessage(message) {
      sent.push(message);
      const request = JSON.parse(message);
      // 先回一条别的 id,模拟并发请求的回包,必须被忽略。
      for (const listener of listeners) listener({ data: JSON.stringify({ id: 'other', ok: true, text: 'wrong' }) });
      const response = reply(request);
      if (response) for (const listener of [...listeners]) listener({ data: JSON.stringify({ id: request.id, ...response }) });
    },
    addEventListener(type, fn) { if (type === 'message') listeners.add(fn); },
    removeEventListener(type, fn) { listeners.delete(fn); },
    get listenerCount() { return listeners.size; },
  };
}

// 场景: 安卓 App 里在密钥输入框点「粘贴」(vivo 等手机对密码框弹出禁止粘贴的安全键盘)。
// 期望: 经 App 注入的 aceAndroid 通道读剪贴板,只认同一个请求 id 的回包,读完摘掉监听;
//       此时不去碰 navigator.clipboard(WebView 不给网页读剪贴板的权限)。
// 回归: 2026-11-08 vivo 上输入 API key 弹安全键盘,既不能粘贴也没有别的入口。
await run('android app reads the clipboard through the aceAndroid bridge', async () => {
  const bridge = fakeAndroidBridge((request) => {
    assert.equal(request.type, 'readClipboard');
    return { ok: true, text: 'sk-from-clipboard' };
  });
  const win = {
    aceAndroid: bridge,
    navigator: { clipboard: { readText: async () => { throw new Error('must not use navigator'); } } },
  };
  assert.equal(clipboardPasteButtonAvailable(win), true);
  const result = await readTextFromSystemClipboard(win);
  assert.deepEqual(result, { ok: true, text: 'sk-from-clipboard', via: 'android' });
  assert.equal(bridge.sent.length, 1);
  assert.equal(bridge.listenerCount, 0);
});

// 场景: App 通道出错或一直不回包(旧版 App、剪贴板被系统拒绝)。
// 期望: 出错原样报 ok:false;超时也报 ok:false 并摘掉监听,不会挂住按钮。
await run('android clipboard bridge failures and timeouts degrade quietly', async () => {
  const failing = fakeAndroidBridge(() => ({ ok: false, error: 'clipboard empty' }));
  assert.deepEqual(await readTextFromSystemClipboard({ aceAndroid: failing }), { ok: false, error: 'clipboard empty' });
  const silent = fakeAndroidBridge(() => null);
  const result = await readTextFromSystemClipboard({ aceAndroid: silent }, { timeoutMs: 5 });
  assert.equal(result.ok, false);
  assert.equal(silent.listenerCount, 0);
});

// 场景: 桌面版 / 普通浏览器。
// 期望: 没有 aceAndroid 通道时不显示「粘贴」按钮(桌面直接 Ctrl+V),读剪贴板仍走 navigator。
await run('paste button stays hidden outside the android app', async () => {
  const win = { navigator: { clipboard: { readText: async () => 'desk' } } };
  assert.equal(clipboardPasteButtonAvailable(win), false);
  assert.equal(clipboardPasteButtonAvailable(undefined), false);
  assert.deepEqual(await readTextFromSystemClipboard(win), { ok: true, text: 'desk', via: 'navigator' });
});
