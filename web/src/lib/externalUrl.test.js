import assert from 'node:assert/strict';
import {
  externalHttpUrlFromNewPageEvent,
  installDesktopExternalLinkRouter,
  openExternalUrl,
} from './externalUrl.js';

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

await run('desktop external URL opener uses native bridge', async () => {
  const calls = [];
  const result = await openExternalUrl('https://github.com/login/device', {
    aceDesktop_openExternalUrl: async (url) => {
      calls.push(url);
      return JSON.stringify({ ok: true });
    },
    open: () => { throw new Error('window.open must not be used'); },
  });

  assert.deepEqual(calls, ['https://github.com/login/device']);
  assert.deepEqual(result, { ok: true, via: 'desktop' });
});

await run('desktop external URL opener does not fallback to window.open on bridge failure', async () => {
  let windowOpenCalls = 0;
  const result = await openExternalUrl('https://github.com/login/device', {
    aceDesktop_openExternalUrl: async () => JSON.stringify({ ok: false, error: 'native failed' }),
    open: () => { windowOpenCalls += 1; },
  });

  assert.equal(result.ok, false);
  assert.equal(result.via, 'desktop');
  assert.equal(result.error, 'native failed');
  assert.equal(windowOpenCalls, 0);
});

await run('external URL opener falls back to window.open outside desktop shell', async () => {
  const calls = [];
  const result = await openExternalUrl('https://github.com/login/device', {
    open: (...args) => { calls.push(args); },
  });

  assert.equal(result.ok, true);
  assert.equal(result.via, 'window-open');
  assert.deepEqual(calls, [['https://github.com/login/device', '_blank', 'noopener,noreferrer']]);
});

function fakeAnchor(href, { download = false } = {}) {
  return {
    href,
    hasAttribute: (name) => name === 'download' && download,
  };
}

function fakeEvent({
  type = 'click',
  button = 0,
  href = 'https://example.com/docs',
  anchor = fakeAnchor(href),
  defaultPrevented = false,
} = {}) {
  const state = { prevented: 0, stopped: 0 };
  return {
    event: {
      type,
      button,
      defaultPrevented,
      target: { closest: () => anchor },
      preventDefault: () => { state.prevented += 1; },
      stopPropagation: () => { state.stopped += 1; },
    },
    state,
  };
}

function fakeDocument() {
  const listeners = new Map();
  const added = [];
  const removed = [];
  return {
    listeners,
    added,
    removed,
    addEventListener(type, handler, capture) {
      listeners.set(type, handler);
      added.push([type, capture]);
    },
    removeEventListener(type, handler, capture) {
      if (listeners.get(type) === handler) listeners.delete(type);
      removed.push([type, capture]);
    },
    dispatch(type, event) {
      listeners.get(type)?.(event);
    },
  };
}

await run('new-page classifier accepts primary and middle-click HTTP links only', () => {
  assert.equal(
    externalHttpUrlFromNewPageEvent(fakeEvent().event),
    'https://example.com/docs',
  );
  assert.equal(
    externalHttpUrlFromNewPageEvent(fakeEvent({ type: 'auxclick', button: 1 }).event),
    'https://example.com/docs',
  );
  assert.equal(externalHttpUrlFromNewPageEvent(fakeEvent({ button: 2 }).event), '');
  assert.equal(
    externalHttpUrlFromNewPageEvent(fakeEvent({ href: 'file:///N:/repo/README.md' }).event),
    '',
  );
  assert.equal(
    externalHttpUrlFromNewPageEvent(fakeEvent({ href: 'javascript:alert(1)' }).event),
    '',
  );
  assert.equal(
    externalHttpUrlFromNewPageEvent(fakeEvent({ anchor: null }).event),
    '',
  );
  assert.equal(
    externalHttpUrlFromNewPageEvent(fakeEvent({
      anchor: fakeAnchor('https://example.com/file', { download: true }),
    }).event),
    '',
  );
});

await run('desktop router prevents WebView popup before opening externally', async () => {
  const doc = fakeDocument();
  const routed = [];
  const { event, state } = fakeEvent();
  const cleanup = installDesktopExternalLinkRouter({
    win: { aceDesktop_openExternalUrl: () => '{}' },
    doc,
    opener: (url) => {
      assert.equal(state.prevented, 1);
      assert.equal(state.stopped, 1);
      routed.push(url);
      return { ok: true };
    },
  });

  assert.deepEqual(doc.added, [['click', true], ['auxclick', true]]);
  doc.dispatch('click', event);
  await Promise.resolve();
  assert.deepEqual(routed, ['https://example.com/docs']);

  cleanup();
  cleanup();
  assert.deepEqual(doc.removed, [['click', true], ['auxclick', true]]);
  doc.dispatch('click', fakeEvent().event);
  assert.equal(routed.length, 1);
});

await run('desktop router reports launcher failure without restoring default navigation', async () => {
  const doc = fakeDocument();
  const failures = [];
  const { event, state } = fakeEvent({ type: 'auxclick', button: 1 });
  installDesktopExternalLinkRouter({
    win: { aceDesktop_openExternalUrl: () => '{}' },
    doc,
    opener: async () => ({ ok: false, error: 'native failed' }),
    onError: (...args) => failures.push(args),
  });

  doc.dispatch('auxclick', event);
  await Promise.resolve();
  await Promise.resolve();
  assert.equal(state.prevented, 1);
  assert.equal(state.stopped, 1);
  assert.deepEqual(failures, [['native failed', 'https://example.com/docs']]);
});

await run('router is not installed outside the Desktop bridge', () => {
  const doc = fakeDocument();
  const cleanup = installDesktopExternalLinkRouter({ win: {}, doc });
  assert.deepEqual(doc.added, []);
  cleanup();
  assert.deepEqual(doc.removed, []);
});
