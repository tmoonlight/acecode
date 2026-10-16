import assert from 'node:assert/strict';
import {
  hasNativeIosSimulatorView,
  nativeFrameFromState,
  nativeSimulatorLayout,
  nativeSimulatorLayoutSignature,
  nativeViewRendering,
  parseNativeViewResult,
  setNativeSimulatorLayout,
  simulatorDeveloperDir,
  unmountNativeSimulatorView,
} from './iosSimulatorNativeView.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

async function runAsync(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function fakeShell(overrides = {}) {
  const calls = [];
  return {
    calls,
    __ACECODE_DESKTOP_SHELL__: true,
    __ACECODE_OS__: 'macos',
    aceDesktop_iosSimulatorViewSetLayout: async (layout) => {
      calls.push(['layout', layout]);
      return JSON.stringify({ ok: true, udid: layout.udid, rendering: false });
    },
    aceDesktop_iosSimulatorViewUnmount: async () => {
      calls.push(['unmount']);
      return '{"ok":true}';
    },
    ...overrides,
  };
}

run('the native view exists only in the macOS Desktop shell with its bridges', () => {
  assert.equal(hasNativeIosSimulatorView(fakeShell()), true);
  assert.equal(hasNativeIosSimulatorView(fakeShell({ __ACECODE_OS__: 'windows' })), false);
  assert.equal(hasNativeIosSimulatorView(fakeShell({ __ACECODE_DESKTOP_SHELL__: false })), false);
  assert.equal(hasNativeIosSimulatorView(fakeShell({ aceDesktop_iosSimulatorViewUnmount: undefined })), false);
  assert.equal(hasNativeIosSimulatorView(undefined), false);
});

run('layout requests carry the canvas rect in CSS pixels', () => {
  const layout = nativeSimulatorLayout({
    udid: 'U',
    developerDir: '/X/Contents/Developer',
    rect: { left: 10.4, top: 20.6, width: 300.2, height: 649.7 },
    visible: true,
    cornerRadius: 41.2,
    occlusionRects: [{ x: 1, y: 2, width: 3, height: 4 }],
  });
  assert.deepEqual(layout, {
    udid: 'U',
    developer_dir: '/X/Contents/Developer',
    x: 10,
    y: 21,
    width: 300,
    height: 650,
    visible: true,
    corner_radius: 41.2,
    occlusion_rects: [{ x: 1, y: 2, width: 3, height: 4 }],
  });
  const hidden = nativeSimulatorLayout({ udid: 'U', developerDir: '/X', rect: null });
  assert.equal(hidden.visible, false);
  assert.equal(hidden.width, 0);
  assert.notEqual(nativeSimulatorLayoutSignature(layout), nativeSimulatorLayoutSignature({ ...layout, visible: false }));
  assert.notEqual(nativeSimulatorLayoutSignature(layout), nativeSimulatorLayoutSignature({ ...layout, occlusion_rects: [] }));
  assert.equal(nativeSimulatorLayoutSignature(layout), nativeSimulatorLayoutSignature({ ...layout }));
});

run('only a rendering state for the same device switches the JPEG stream off', () => {
  assert.equal(nativeViewRendering({ udid: 'U', rendering: true }, 'U'), true);
  assert.equal(nativeViewRendering({ udid: 'U', rendering: false }, 'U'), false);
  assert.equal(nativeViewRendering({ udid: 'V', rendering: true }, 'U'), false);
  assert.equal(nativeViewRendering(null, 'U'), false);
  assert.equal(nativeViewRendering({ udid: '', rendering: true }, ''), false);
});

run('pointer mapping uses the native framebuffer turned to the interface', () => {
  assert.deepEqual(nativeFrameFromState({ pixel_width: 1170, pixel_height: 2532, rotation: 0 }),
    { width: 1170, height: 2532, rotation: 0 });
  assert.deepEqual(nativeFrameFromState({ pixel_width: 1170, pixel_height: 2532, rotation: 90 }),
    { width: 2532, height: 1170, rotation: 90 });
  assert.deepEqual(nativeFrameFromState({ pixel_width: 1170, pixel_height: 2532, rotation: 270 }),
    { width: 2532, height: 1170, rotation: 270 });
  assert.deepEqual(nativeFrameFromState({ pixel_width: 10, pixel_height: 20, rotation: 45 }),
    { width: 10, height: 20, rotation: 0 });
  assert.equal(nativeFrameFromState({ pixel_width: 0, pixel_height: 20 }), null);
  assert.equal(nativeFrameFromState(null), null);
});

run('the developer directory comes from the selected Xcode', () => {
  assert.equal(simulatorDeveloperDir({ xcode: { selected: { developer_dir: '/A/Contents/Developer' } } }),
    '/A/Contents/Developer');
  assert.equal(simulatorDeveloperDir({ xcode: { selected: null }, helper: { developer_dir: '/B' } }), '/B');
  assert.equal(simulatorDeveloperDir(null), '');
  assert.deepEqual(parseNativeViewResult('{"ok":true}'), { ok: true });
  assert.deepEqual(parseNativeViewResult('nope'), {});
  assert.deepEqual(parseNativeViewResult({ ok: false }), { ok: false });
});

await runAsync('only the panel that placed the view may take it down', async () => {
  const shell = fakeShell();
  const first = {};
  const second = {};
  const placed = await setNativeSimulatorLayout({ udid: 'U' }, first, shell);
  assert.equal(placed.ok, true);
  await setNativeSimulatorLayout({ udid: 'V' }, second, shell);
  const skipped = await unmountNativeSimulatorView(first, shell);
  assert.equal(skipped.skipped, true);
  assert.deepEqual(shell.calls.map(([kind]) => kind), ['layout', 'layout']);
  await unmountNativeSimulatorView(second, shell);
  assert.deepEqual(shell.calls.map(([kind]) => kind), ['layout', 'layout', 'unmount']);
  const unsupported = await setNativeSimulatorLayout({ udid: 'U' }, first, { __ACECODE_OS__: 'linux' });
  assert.equal(unsupported.supported, false);
  const failing = fakeShell({ aceDesktop_iosSimulatorViewSetLayout: async () => { throw new Error('boom'); } });
  assert.deepEqual(await setNativeSimulatorLayout({ udid: 'U' }, first, failing), { ok: false, error: 'boom' });
});
