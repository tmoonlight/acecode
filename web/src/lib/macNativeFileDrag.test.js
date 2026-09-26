import { readCppSource } from './cppSourcePaths.testHelper.js';
import assert from 'node:assert/strict';
import {
  installNativeFileDropRouter,
  nativeFileDropTarget,
  nativeFileDropViewportPoint,
  registerNativeComposerFileDrop,
  routeNativeFileDrop,
} from './macNativeFileDrag.js';

function run(name, fn) {
  try {
    fn();
    console.log(`  PASS ${name}`);
  } catch (error) {
    console.error(`  FAIL ${name}`);
    throw error;
  }
}

function element({ composer = null, terminal = null } = {}) {
  return {
    closest(selector) {
      if (selector === '.ace-composer-card') return composer;
      if (selector === '.ace-console-term') return terminal;
      return null;
    },
  };
}

run('converts normalized native coordinates to CSS viewport coordinates', () => {
  assert.deepEqual(
    nativeFileDropViewportPoint({ xRatio: 0.25, yRatio: 0.75 }, { width: 1200, height: 800 }),
    { x: 300, y: 600 },
  );
  assert.deepEqual(
    nativeFileDropViewportPoint({ xRatio: 0, yRatio: 0 }, { width: 1200, height: 800 }),
    { x: 0, y: 0 },
  );
});

run('rejects missing, non-finite, and half-open upper-bound coordinates', () => {
  for (const location of [
    null,
    { xRatio: null, yRatio: 0.5 },
    { xRatio: '', yRatio: 0.5 },
    { xRatio: -0.01, yRatio: 0.5 },
    { xRatio: 1, yRatio: 0.5 },
    { xRatio: 0.5, yRatio: 1 },
    { xRatio: Number.NaN, yRatio: 0.5 },
  ]) {
    assert.equal(nativeFileDropViewportPoint(location, { width: 100, height: 100 }), null);
  }
});

run('selects only eligible composer or visible terminal targets', () => {
  const composer = { dataset: {} };
  assert.deepEqual(nativeFileDropTarget(element({ composer })), { kind: 'composer', element: composer });
  const nestedOverlay = {
    closest(selector) {
      if (selector === '[data-ace-native-overlay]') return this;
      if (selector === '.ace-composer-card') return { dataset: {} };
      return null;
    },
  };
  assert.equal(nativeFileDropTarget(nestedOverlay), null);
  assert.equal(nativeFileDropTarget(element({ composer: {
    dataset: { nativeFileDropDisabled: 'true' },
  } })), null);
  assert.deepEqual(nativeFileDropTarget(element({ terminal: {
    dataset: { tabId: 'tab-1' }, style: { display: 'block' },
  } })), { kind: 'console', tabId: 'tab-1' });
  assert.equal(nativeFileDropTarget(element({ terminal: {
    dataset: { tabId: 'tab-2' }, style: { display: 'none' },
  } })), null);
});

run('routes a coordinate-bearing drop to composer exactly once', () => {
  const calls = [];
  const composer = { dataset: {} };
  const unregister = registerNativeComposerFileDrop(composer, (payload) => calls.push(['composer', payload]));
  const win = {
    innerWidth: 1000,
    innerHeight: 500,
    __aceComposerAcceptFileDrop: () => { throw new Error('Coordinate routing must use the hit composer'); },
    __aceConsoleAcceptFileDrop: (payload) => calls.push(['console', payload]),
  };
  const doc = {
    elementFromPoint: (x, y) => {
      assert.equal(x, 500);
      assert.equal(y, 125);
      return element({ composer });
    },
  };
  assert.equal(routeNativeFileDrop({
    paths: ['/tmp/a'], location: { xRatio: 0.5, yRatio: 0.25 },
  }, win, doc), true);
  assert.deepEqual(calls, [['composer', { paths: ['/tmp/a'], nativeLocation: true }]]);
  unregister();
});

run('multiple composers receive only their own drops and stale cleanup preserves the current receiver', () => {
  const first = { dataset: {} };
  const second = { dataset: {} };
  const calls = [];
  const removeOld = registerNativeComposerFileDrop(first, () => calls.push('stale'));
  const removeFirst = registerNativeComposerFileDrop(first, () => calls.push('first'));
  const removeSecond = registerNativeComposerFileDrop(second, () => calls.push('second'));
  removeOld();
  const win = { innerWidth: 100, innerHeight: 100 };
  const drop = { paths: ['/tmp/a'], location: { xRatio: 0.5, yRatio: 0.5 } };
  const doc = (composer) => ({ elementFromPoint: () => element({ composer }) });
  assert.equal(routeNativeFileDrop(drop, win, doc(first)), true);
  assert.equal(routeNativeFileDrop(drop, win, doc(second)), true);
  removeSecond();
  assert.equal(routeNativeFileDrop(drop, win, doc(first)), true);
  assert.equal(routeNativeFileDrop(drop, win, doc(second)), false);
  assert.deepEqual(calls, ['first', 'second', 'first']);
  removeFirst();
});

run('uses the topmost element and rejects overlays or invalid coordinates', () => {
  let called = false;
  const win = {
    innerWidth: 100,
    innerHeight: 100,
    __aceComposerAcceptFileDrop: () => { called = true; },
  };
  assert.equal(routeNativeFileDrop({
    paths: ['/tmp/a'], location: { xRatio: 0.5, yRatio: 0.5 },
  }, win, { elementFromPoint: () => element() }), false);
  assert.equal(routeNativeFileDrop({ paths: ['/tmp/a'] }, win, {
    elementFromPoint: () => element({ composer: { dataset: {} } }),
  }), false);
  assert.equal(called, false);
});

run('native bridge rejects invalid required coordinates instead of legacy fallback', () => {
  const main = readCppSource('desktop/main.cpp');
  const host = readCppSource('desktop/web_host.cpp');
  assert.match(host, /FileDropContext\{location, true\}/);
  assert.match(main, /if\(" \+ coordinate_required \+ "\)\{return;\}/);
  assert.ok(main.indexOf('if(" + coordinate_required + "){return;}') < main.indexOf('var legacy=p.paths;'));
  assert.match(main, /target\\\":\\\"router.*reason\\\":\\\"receiver-exception/);
});

run('routes a terminal drop with its tab id and installs cleanly', () => {
  const calls = [];
  const win = {
    innerWidth: 100,
    innerHeight: 100,
    __aceConsoleAcceptFileDrop: (payload) => calls.push(payload),
  };
  const doc = { elementFromPoint: () => element({ terminal: {
    dataset: { tabId: 'active-tab' }, style: { display: 'block' },
  } }) };
  const remove = installNativeFileDropRouter(win, doc);
  assert.equal(win.__aceRouteNativeFileDrop({
    paths: ['/tmp/a'], location: { xRatio: 0.2, yRatio: 0.3 },
  }), true);
  assert.deepEqual(calls, [{ paths: ['/tmp/a'], nativeLocation: true, tabId: 'active-tab' }]);
  remove();
  assert.equal(win.__aceRouteNativeFileDrop, undefined);
});
