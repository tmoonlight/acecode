import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  bindDesktopComposerAutoFocus,
  isComposerEditorFocused,
  preserveComposerFocusOnPointerDown,
  requestDesktopFileDragActivation,
  requestDesktopFileDropFocus,
  requestDesktopWindowFocus,
  restoreComposerTextareaCaret,
  shouldAutoFocusDesktopComposer,
  shouldRestoreComposerTextareaFocus,
} from './composerCaretRestore.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function element(name) {
  const children = new Set();
  return {
    name,
    appendChild(child) {
      children.add(child);
      return child;
    },
    contains(target) {
      return target === this || children.has(target);
    },
  };
}

function textarea(value = '') {
  const el = element('textarea');
  return Object.assign(el, {
    value,
    selectionStart: 0,
    selectionEnd: 0,
    selectionDirection: 'none',
    focusCalls: 0,
    selectionCalls: [],
    focus() {
      this.focusCalls += 1;
    },
    setSelectionRange(start, end, direction) {
      this.selectionStart = start;
      this.selectionEnd = end;
      this.selectionDirection = direction;
      this.selectionCalls.push({ start, end, direction });
    },
  });
}

function eventTarget() {
  const listeners = new Map();
  return {
    addEventListener(type, listener) {
      if (!listeners.has(type)) listeners.set(type, new Set());
      listeners.get(type).add(listener);
    },
    removeEventListener(type, listener) {
      listeners.get(type)?.delete(listener);
    },
    dispatch(type, event = {}) {
      for (const listener of listeners.get(type) || []) listener({ ...event, type });
    },
    listenerCount(type) {
      return listeners.get(type)?.size || 0;
    },
  };
}

function terminalFocusTarget({ collapsed = false, connected = true } = {}) {
  const region = {
    isConnected: connected,
    getAttribute(name) {
      return name === 'data-collapsed' ? String(collapsed) : null;
    },
  };
  return Object.assign(element('terminal-textarea'), {
    closest(selector) {
      return selector === '[data-ace-focus-region="terminal"]' ? region : null;
    },
  });
}

function composerFocusFixture() {
  const root = element('composer-card');
  const editor = root.appendChild(element('editor'));
  editor.getAttribute = (name) => name === 'data-ace-rich-composer' ? 'true' : null;
  const documentRef = { activeElement: editor };
  root.ownerDocument = documentRef;
  const mouseDown = (target, overrides = {}) => ({
    target,
    button: 0,
    defaultPrevented: false,
    preventDefault() { this.defaultPrevented = true; },
    ...overrides,
  });
  return { root, editor, documentRef, mouseDown };
}

run('composer card controls and portal menus prevent the default mouse focus transfer', () => {
  const { root, editor, documentRef, mouseDown } = composerFocusFixture();
  const button = root.appendChild(element('button'));
  const portalOption = element('portal-option');
  assert.equal(root.contains(portalOption), false);
  for (const target of [root, button, element('button-svg'), portalOption]) {
    const event = mouseDown(target);
    assert.equal(preserveComposerFocusOnPointerDown(event, root), true);
    assert.equal(event.defaultPrevented, true);
    assert.equal(documentRef.activeElement, editor);
  }
});

run('composer focus preservation leaves editor mouse selection and secondary clicks alone', () => {
  const { root, editor, mouseDown } = composerFocusFixture();
  const text = editor.appendChild(element('text'));
  for (const event of [mouseDown(editor), mouseDown(text), mouseDown(root, { button: 1 }), mouseDown(root, { button: 2 }), mouseDown(root, { isPrimary: false })]) {
    assert.equal(preserveComposerFocusOnPointerDown(event, root), false);
    assert.equal(event.defaultPrevented, false);
  }
});

run('composer controls keep normal focus behavior when the local editor is not focused', () => {
  const { root, documentRef, mouseDown } = composerFocusFixture();
  const otherEditor = element('other-composer');
  otherEditor.getAttribute = () => 'true';
  for (const activeElement of [null, element('body'), root.appendChild(element('button')), otherEditor]) {
    documentRef.activeElement = activeElement;
    assert.equal(isComposerEditorFocused(root), false);
    const event = mouseDown(root);
    assert.equal(preserveComposerFocusOnPointerDown(event, root), false);
    assert.equal(event.defaultPrevented, false);
  }
  assert.equal(isComposerEditorFocused(null), false);
});

run('composer focus preservation allows other editable controls to receive focus', () => {
  const { root, mouseDown } = composerFocusFixture();
  for (const name of ['input', 'textarea', 'select', 'contenteditable', 'textbox']) {
    const field = root.appendChild(element(name));
    field.closest = () => field;
    const event = mouseDown(field);
    assert.equal(preserveComposerFocusOnPointerDown(event, root), false);
    assert.equal(event.defaultPrevented, false);
  }
});

run('composer focus preservation respects an already handled mouse event', () => {
  const { root, mouseDown } = composerFocusFixture();
  const event = mouseDown(root, {
    defaultPrevented: true,
    preventDefault() { throw new Error('already handled'); },
  });
  assert.equal(preserveComposerFocusOnPointerDown(event, root), false);
});

run('composer caret restore is allowed when focus fell back to body', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  input.selectionStart = 5;
  input.selectionEnd = 5;

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 2, end: 2, direction: 'none' },
    documentRef: { activeElement: body, body, documentElement: html },
  });

  assert.equal(restored, true);
  assert.equal(input.focusCalls, 1);
  assert.deepEqual(input.selectionCalls.at(-1), { start: 2, end: 2, direction: 'none' });
});

run('composer caret restore is skipped for focus outside the composer', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  const externalInput = textarea('outside');

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 2, end: 2, direction: 'none' },
    documentRef: { activeElement: externalInput, body, documentElement: html },
  });

  assert.equal(restored, false);
  assert.equal(input.focusCalls, 0);
  assert.equal(input.selectionCalls.length, 0);
});

run('composer caret restore can intentionally focus from an external drop source', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  const externalButton = element('button');

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 3, end: 3, direction: 'none' },
    documentRef: { activeElement: externalButton, body, documentElement: html },
    allowExternalFocus: true,
  });

  assert.equal(restored, true);
  assert.equal(input.focusCalls, 1);
  assert.deepEqual(input.selectionCalls.at(-1), { start: 3, end: 3, direction: 'none' });
});

run('composer caret restore uses live selection when textarea is already active', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  input.selectionStart = 4;
  input.selectionEnd = 4;

  const restored = restoreComposerTextareaCaret({
    textareaElement: input,
    rootElement: root,
    selection: { start: 1, end: 1, direction: 'none' },
    documentRef: { activeElement: input, body, documentElement: html },
  });

  assert.equal(restored, true);
  assert.deepEqual(input.selectionCalls.at(-1), { start: 4, end: 4, direction: 'none' });
});

run('composer focus restore allows active descendants inside composer root', () => {
  const body = element('body');
  const html = element('html');
  const root = element('root');
  const input = root.appendChild(textarea('hello'));
  const addButton = root.appendChild(element('button'));

  assert.equal(shouldRestoreComposerTextareaFocus({
    activeElement: addButton,
    rootElement: root,
    textareaElement: input,
    bodyElement: body,
    documentElement: html,
  }), true);
});

run('desktop composer auto-focus is enabled only for an unobstructed visible shell chat', () => {
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'shell',
    chatVisible: true,
    blockingSurfaceOpen: false,
  }), true);
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'browser',
    chatVisible: true,
    blockingSurfaceOpen: false,
  }), false);
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'shell',
    chatVisible: false,
    blockingSurfaceOpen: false,
  }), false);
  assert.equal(shouldAutoFocusDesktopComposer({
    desktopMode: 'shell',
    chatVisible: true,
    blockingSurfaceOpen: true,
  }), false);
});

run('integrated terminal advertises the desktop focus region contract', () => {
  const source = readFileSync(new URL('../components/ConsoleDock.jsx', import.meta.url), 'utf8');
  assert.match(
    source,
    /className="ace-console-dock"\s+data-ace-focus-region="terminal"/,
  );
});

function focusFixture() {
  const win = eventTarget();
  const frames = new Map();
  const timers = new Map();
  let id = 0;
  Object.assign(win, {
    requestAnimationFrame: (fn) => { frames.set(++id, fn); return id; },
    cancelAnimationFrame: (key) => frames.delete(key),
    setTimeout: (fn) => { timers.set(++id, fn); return id; },
    clearTimeout: (key) => timers.delete(key),
  });
  const documentRef = Object.assign(eventTarget(), {
    visibilityState: 'visible', querySelector: () => null, hasFocus: () => true,
  });
  const calls = [];
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: true, win, documentRef, onFocus: (guard) => calls.push(guard),
  });
  const flush = (queue) => { const entries = [...queue.values()]; queue.clear(); entries.forEach(fn => fn()); };
  return { win, documentRef, calls, cleanup,
    frame: () => flush(frames), timer: () => flush(timers),
    restore: () => { flush(frames); flush(frames); },
  };
}

run('activation notifications coalesce and cleanup cancels pending restore', () => {
  const f = focusFixture();
  f.win.dispatch('focus');
  f.win.dispatch('pageshow');
  f.win.dispatch('acecode:desktop-window-focus');
  f.restore();
  assert.equal(f.calls.length, 1);
  f.cleanup();
  f.timer();
  assert.equal(f.calls.length, 1);
  assert.equal(f.calls[0](), false);
  for (const type of ['focus', 'blur', 'pageshow', 'acecode:desktop-window-focus']) {
    assert.equal(f.win.listenerCount(type), 0);
  }
  for (const type of ['visibilitychange', 'pointerdown', 'keydown', 'beforeinput', 'input', 'compositionstart', 'compositionend']) {
    assert.equal(f.documentRef.listenerCount(type), 0);
  }
});

run('activation always returns to chat even after using the terminal', () => {
  const f = focusFixture();
  f.documentRef.activeElement = terminalFocusTarget();
  f.win.dispatch('acecode:desktop-window-focus');
  f.restore();
  assert.equal(f.calls.length, 1);
  f.cleanup();
});

run('hidden and background pages never restore from visibility alone', () => {
  const f = focusFixture();
  f.documentRef.visibilityState = 'hidden';
  f.win.dispatch('focus'); f.restore();
  assert.equal(f.calls.length, 0);
  f.documentRef.visibilityState = 'visible';
  f.documentRef.hasFocus = () => false;
  f.documentRef.dispatch('visibilitychange'); f.restore();
  assert.equal(f.calls.length, 0);
  f.documentRef.hasFocus = () => true;
  f.documentRef.dispatch('visibilitychange'); f.restore();
  assert.equal(f.calls.length, 1);
  f.cleanup();
});

{
  const f = focusFixture();
  let nativeCalls = 0;
  f.win.aceDesktop_focusActiveWebview = () => {
    nativeCalls += 1;
    f.documentRef.hasFocus = () => true;
    return { ok: true };
  };
  f.documentRef.visibilityState = 'hidden';
  f.documentRef.hasFocus = () => false;
  f.win.dispatch('acecode:desktop-window-focus'); f.restore();
  assert.equal(nativeCalls, 0);
  f.documentRef.visibilityState = 'visible';
  f.documentRef.dispatch('visibilitychange'); f.frame();
  await Promise.resolve();
  f.frame();
  assert.equal(nativeCalls, 1);
  assert.equal(f.calls.length, 1);
  f.cleanup();
  console.log('[pass] native activation arriving before visibility restores the unfocused WebView');
}

for (const event of ['blur', 'pointerdown', 'keydown', 'beforeinput', 'input', 'compositionstart', 'compositionend', 'visibilitychange']) {
  run(`${event} cancels delayed focus and the Slate retry guard`, () => {
    const f = focusFixture();
    f.win.dispatch('focus'); f.restore();
    assert.equal(f.calls.length, 1);
    if (event === 'visibilitychange') f.documentRef.visibilityState = 'hidden';
    (event === 'blur' ? f.win : f.documentRef).dispatch(event);
    f.timer();
    assert.equal(f.calls.length, 1);
    assert.equal(f.calls[0](), false);
    f.cleanup();
  });
}

run('a modal opened during the pending restore blocks focus', () => {
  const f = focusFixture();
  f.win.dispatch('focus');
  f.documentRef.querySelector = () => ({ role: 'dialog' });
  f.restore(); f.timer();
  assert.equal(f.calls.length, 0);
  f.cleanup();
});

run('activation during IME never reaches the native focus bridge and resumes after composition ends', () => {
  const f = focusFixture();
  let nativeCalls = 0;
  f.win.aceDesktop_focusActiveWebview = () => { nativeCalls += 1; return {ok:true}; };
  f.documentRef.dispatch('compositionstart');
  f.win.dispatch('acecode:desktop-window-focus');
  f.win.dispatch('focus');
  f.restore(); f.timer();
  assert.equal(nativeCalls, 0);
  assert.equal(f.calls.length, 0);
  f.documentRef.dispatch('compositionend');
  f.documentRef.hasFocus = () => false;
  f.win.dispatch('acecode:desktop-window-focus');
  f.frame();
  assert.equal(nativeCalls, 1);
  f.cleanup();
});

run('an already focused document restores chat without repeating native MoveFocus', () => {
  const f = focusFixture();
  let nativeCalls = 0;
  f.win.aceDesktop_focusActiveWebview = () => { nativeCalls += 1; return {ok:true}; };
  f.win.dispatch('acecode:desktop-window-focus');
  f.restore();
  assert.equal(nativeCalls, 0);
  assert.equal(f.calls.length, 1);
  f.cleanup();
});

for (const interruptedBy of ['blur', 'hidden']) {
  run(`composition interrupted by ${interruptedBy} cannot block the next activation`, () => {
    const f = focusFixture();
    f.documentRef.dispatch('compositionstart');
    if (interruptedBy === 'blur') f.win.dispatch('blur');
    else {
      f.documentRef.visibilityState = 'hidden';
      f.documentRef.dispatch('visibilitychange');
    }
    f.documentRef.visibilityState = 'visible';
    f.win.dispatch('acecode:desktop-window-focus');
    f.restore();
    assert.equal(f.calls.length, 1);
    f.cleanup();
  });
}

for (const outcome of ['success', 'background', 'reject', 'cleanup', 'blur', 'modal', 'compositionstart', 'beforeinput']) {
  const f = focusFixture();
  let resolve, reject;
  let nativeCalls = 0;
  f.win.aceDesktop_focusActiveWebview = () => {
    nativeCalls += 1;
    return new Promise((yes, no) => { resolve = yes; reject = no; });
  };
  f.documentRef.hasFocus = () => false;
  f.win.dispatch('acecode:desktop-window-focus'); f.frame();
  assert.equal(nativeCalls, 1);
  assert.equal(f.calls.length, 0);
  f.win.dispatch('focus');
  if (outcome === 'cleanup') f.cleanup();
  if (outcome === 'blur') f.win.dispatch('blur');
  if (outcome === 'compositionstart' || outcome === 'beforeinput') f.documentRef.dispatch(outcome);
  if (outcome === 'modal') f.documentRef.querySelector = () => ({ role: 'dialog' });
  if (outcome === 'reject') reject(new Error('closed'));
  else {
    f.documentRef.hasFocus = () => true;
    resolve({ ok: outcome !== 'background' });
  }
  await Promise.resolve();
  f.restore(); f.timer();
  assert.equal(f.calls.length, outcome === 'success' ? 2 : 0, outcome);
  assert.equal(nativeCalls, 1);
  f.cleanup();
  console.log(`[pass] native focus handoff: ${outcome}`);
}

run('desktop composer auto-focus does not steal focus through a modal surface', () => {
  const f = focusFixture();
  f.documentRef.querySelector = () => ({ role: 'dialog' });
  f.win.dispatch('focus'); f.restore();
  assert.equal(f.calls.length, 0);
  f.cleanup();
});

run('desktop composer auto-focus stays inert when disabled', () => {
  const win = eventTarget();
  const documentRef = Object.assign(eventTarget(), {
    visibilityState: 'visible',
    querySelector: () => null,
  });
  const cleanup = bindDesktopComposerAutoFocus({
    enabled: false,
    onFocus: () => { throw new Error('must not focus'); },
    win,
    documentRef,
  });

  assert.equal(win.listenerCount('focus'), 0);
  cleanup();
});

run('desktop window focus request calls optional bridge', () => {
  let calls = 0;
  const ok = requestDesktopWindowFocus({
    aceDesktop_focusWindow: () => {
      calls += 1;
      return Promise.resolve('{"ok":true}');
    },
  });

  assert.equal(ok, true);
  assert.equal(calls, 1);
});

run('desktop window focus request is a no-op without bridge', () => {
  assert.equal(requestDesktopWindowFocus({}), false);
  assert.equal(requestDesktopWindowFocus(null), false);
});

run('desktop window focus request swallows bridge throw', () => {
  const ok = requestDesktopWindowFocus({
    aceDesktop_focusWindow: () => {
      throw new Error('native unavailable');
    },
  });

  assert.equal(ok, false);
});

run('desktop file drag activation calls its dedicated optional bridge once', () => {
  let calls = 0;
  const ok = requestDesktopFileDragActivation({
    aceDesktop_activateFileDropWindow: () => {
      calls += 1;
      return Promise.resolve('{"ok":true}');
    },
  });

  assert.equal(ok, true);
  assert.equal(calls, 1);
  assert.equal(requestDesktopFileDragActivation({}), false);
});

run('desktop file drag activation swallows bridge throw', () => {
  const ok = requestDesktopFileDragActivation({
    aceDesktop_activateFileDropWindow: () => {
      throw new Error('native unavailable');
    },
  });

  assert.equal(ok, false);
});

run('file drop prefers keyboard focus restoration without notification activation', () => {
  const calls = [];
  assert.equal(requestDesktopFileDropFocus({
    aceDesktop_focusFileDropWindow: () => calls.push('drop'),
    aceDesktop_activateFileDropWindow: () => calls.push('entry'),
    aceDesktop_focusWindow: () => calls.push('notification'),
  }), true);
  assert.deepEqual(calls, ['drop']);
});

run('older shells retry drag activation once; missing and failed bridges stay optional', () => {
  let calls = 0;
  assert.equal(requestDesktopFileDropFocus({aceDesktop_activateFileDropWindow: () => { calls++; }}), true);
  assert.equal(calls, 1);
  assert.equal(requestDesktopFileDropFocus({}), false);
  assert.equal(requestDesktopFileDropFocus({aceDesktop_focusFileDropWindow: () => { throw new Error('unavailable'); }}), false);
});
