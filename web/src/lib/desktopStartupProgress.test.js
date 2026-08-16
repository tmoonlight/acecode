import assert from 'node:assert/strict';
import {
  DESKTOP_STARTUP_PROGRESS_EVENT,
  initialDesktopStartupProgress,
  installDesktopStartupPaintReporter,
  normalizeDesktopStartupEvent,
  normalizeDesktopStartupSnapshot,
  reportDesktopStartupMilestone,
  subscribeDesktopStartupProgress,
} from './desktopStartupProgress.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function event(sequence, overrides = {}) {
  return {
    sequence,
    stage: 'webview_navigate_begin',
    message: '正在加载首屏…',
    source: 'native',
    elapsed_ms: 1250,
    terminal: false,
    ...overrides,
  };
}

function snapshot(current = event(1), history = [current]) {
  return { version: 1, current, history };
}

function makeWindow(initial = snapshot()) {
  const listeners = new Map();
  const calls = [];
  return {
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_DESKTOP_STARTUP__: initial,
    performance: {
      now: () => 321.5,
      getEntriesByName: () => [],
    },
    aceDesktop_reportStartupMilestone(payload) {
      calls.push(payload);
      return Promise.resolve('{"ok":true}');
    },
    addEventListener(name, listener) { listeners.set(name, listener); },
    removeEventListener(name, listener) {
      if (listeners.get(name) === listener) listeners.delete(name);
    },
    emit(name, detail) { listeners.get(name)?.({ detail }); },
    calls,
    listeners,
  };
}

run('normalizes a bounded startup event and rejects free-form payloads', () => {
  assert.deepEqual(normalizeDesktopStartupEvent(event(1)), event(1));
  assert.equal(normalizeDesktopStartupEvent({ ...event(1), stage: '../../token' }), null);
  assert.equal(normalizeDesktopStartupEvent({ ...event(1), message: 'x'.repeat(161) }), null);
  assert.equal(normalizeDesktopStartupSnapshot({ version: 2, current: event(1) }), null);
});

run('reads bootstrap snapshots only inside the Desktop shell', () => {
  const win = makeWindow();
  assert.equal(initialDesktopStartupProgress(win).current.sequence, 1);
  assert.equal(initialDesktopStartupProgress({ __ACECODE_DESKTOP_STARTUP__: snapshot() }), null);
});

run('subscribes to native progress snapshots and ignores malformed events', () => {
  const win = makeWindow();
  const received = [];
  const unsubscribe = subscribeDesktopStartupProgress((value) => received.push(value), win);
  win.emit(DESKTOP_STARTUP_PROGRESS_EVENT, snapshot(event(2, { stage: 'dom_ready' })));
  win.emit(DESKTOP_STARTUP_PROGRESS_EVENT, { version: 1, current: { stage: 'bad' } });
  assert.equal(received.length, 1);
  assert.equal(received[0].current.stage, 'dom_ready');
  assert.equal(win.__ACECODE_DESKTOP_STARTUP__.current.sequence, 2);
  unsubscribe();
  assert.equal(win.listeners.has(DESKTOP_STARTUP_PROGRESS_EVENT), false);
});

run('reports allowlisted milestones once and never requires a bridge in browser mode', () => {
  const win = makeWindow();
  assert.equal(reportDesktopStartupMilestone('daemon_connecting', win), true);
  assert.equal(reportDesktopStartupMilestone('daemon_connecting', win), false);
  assert.equal(reportDesktopStartupMilestone('arbitrary_log', win), false);
  assert.deepEqual(win.calls, [{ stage: 'daemon_connecting', performance_ms: 321.5 }]);
  assert.equal(reportDesktopStartupMilestone('ui_ready', { performance: win.performance }), false);
});

run('reports an already buffered first-contentful-paint without installing an observer', () => {
  const win = makeWindow();
  win.performance.getEntriesByName = () => [
    { name: 'first-contentful-paint', startTime: 456.25 },
  ];
  const cleanup = installDesktopStartupPaintReporter(win);
  assert.deepEqual(win.calls, [
    { stage: 'first_contentful_paint', performance_ms: 456.25 },
  ]);
  cleanup();
});

run('observes a future first-contentful-paint and disconnects afterwards', () => {
  const win = makeWindow();
  let callback;
  let disconnected = 0;
  win.PerformanceObserver = class {
    constructor(next) { callback = next; }
    observe(options) { assert.equal(options.type, 'paint'); }
    disconnect() { disconnected += 1; }
  };
  installDesktopStartupPaintReporter(win);
  callback({ getEntries: () => [{ name: 'first-contentful-paint', startTime: 99 }] });
  assert.equal(win.calls[0].stage, 'first_contentful_paint');
  assert.equal(disconnected, 1);
});
