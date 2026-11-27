import assert from 'node:assert/strict';
import { focusAnnotationDescription } from './annotationFocus.js';

const tick = () => new Promise((resolve) => setImmediate(resolve));

function harness({ native = true } = {}) {
  const frames = [];
  const timers = [];
  const listeners = new Map();
  const input = {
    isConnected: true,
    value: 'existing text',
    focused: 0,
    selection: null,
    focus() {
      this.focused += 1;
      documentRef.activeElement = this;
    },
    setSelectionRange(start, end) {
      this.selection = [start, end];
    },
  };
  const documentRef = {
    activeElement: null,
    addEventListener(type, listener) {
      const entries = listeners.get(type) || [];
      entries.push(listener);
      listeners.set(type, entries);
    },
    removeEventListener(type, listener) {
      listeners.set(type, (listeners.get(type) || []).filter((item) => item !== listener));
    },
  };
  let nativeCalls = 0;
  const win = {
    requestAnimationFrame(callback) {
      frames.push(callback);
      return frames.length;
    },
    cancelAnimationFrame(id) {
      frames[id - 1] = null;
    },
    setTimeout(callback) {
      timers.push(callback);
      return timers.length;
    },
    clearTimeout(id) {
      timers[id - 1] = null;
    },
  };
  if (native) {
    win.aceDesktop_focusActiveWebview = () => {
      nativeCalls += 1;
      return Promise.resolve({ ok: true });
    };
  }
  return {
    input,
    documentRef,
    win,
    listeners,
    frames,
    timers,
    nativeCalls: () => nativeCalls,
  };
}

{
  const test = harness();
  const cancel = focusAnnotationDescription(test.input, {
    win: test.win,
    documentRef: test.documentRef,
  });
  await tick();
  assert.equal(test.nativeCalls(), 1, 'the native page releases focus before DOM focus');
  assert.equal(test.input.focused, 0);
  test.frames.shift()();
  assert.equal(test.input.focused, 1);
  assert.deepEqual(test.input.selection, [13, 13], 'the caret lands at the end');
  test.timers.shift()();
  assert.equal(test.input.focused, 2, 'one retry covers late native focus restoration');
  assert.equal(test.listeners.get('keydown')?.length, 0, 'focus retry listeners are released');
  cancel();
}

{
  const test = harness();
  focusAnnotationDescription(test.input, { win: test.win, documentRef: test.documentRef });
  await tick();
  test.listeners.get('pointerdown')[0]({ target: {} });
  test.frames.shift()?.();
  test.timers.shift()?.();
  assert.equal(test.input.focused, 0, 'user interaction elsewhere cancels a queued focus');
}

{
  const test = harness({ native: false });
  focusAnnotationDescription(test.input, { win: test.win, documentRef: test.documentRef });
  test.frames.shift()();
  assert.equal(test.input.focused, 1, 'web-only hosts still focus the description');
}

console.log('[pass] annotation description focus returns native focus before placing the caret');
