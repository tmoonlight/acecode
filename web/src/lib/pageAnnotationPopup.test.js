import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { transformWithEsbuild } from 'vite';
import * as dialogKeyboard from './dialogKeyboard.js';
import * as pageAnnotations from './pageAnnotations.js';
import * as shortcutBindings from './shortcutBindings.js';
import * as annotationSubmission from './annotationSubmission.js';
import * as annotationFocus from './annotationFocus.js';

const filename = new URL('../components/PageAnnotations.jsx', import.meta.url);
const compiled = await transformWithEsbuild(fs.readFileSync(filename, 'utf8'), filename.pathname, {
  loader: 'jsx', format: 'cjs', jsx: 'transform', jsxFactory: 'React.createElement',
});

function fixture(command) {
  const slots = [];
  const timers = new Map();
  const cleanups = [];
  const toasts = [];
  let slot = 0;
  let sequence = 0;
  const React = {
    createElement: (type, props, ...children) => ({ type, props: { ...props, children } }),
    useRef(value) { const index = slot++; return slots[index] ??= { current: value }; },
    useState(value) {
      const index = slot++;
      if (!(index in slots)) slots[index] = value;
      return [slots[index], next => { slots[index] = typeof next === 'function' ? next(slots[index]) : next; }];
    },
    useEffect(effect, deps) {
      const index = slot++;
      if (!(index in slots)) { slots[index] = deps; cleanups.push(effect()); }
    },
    useLayoutEffect() { slot++; },
  };
  const context = vm.createContext({
    React, module: { exports: {} },
    require: name => name === 'react' ? React : name.endsWith('/dialogKeyboard.js') ? dialogKeyboard
      : name.endsWith('/pageAnnotations.js') ? pageAnnotations
        : name.endsWith('/annotationSubmission.js') ? annotationSubmission
        : name.endsWith('/annotationFocus.js') ? annotationFocus
        : name.endsWith('/Toast.jsx') ? { toast: options => toasts.push(options) }
        : name.endsWith('/shortcutBindings.js') ? shortcutBindings : { notifyNativeSurfaceOverlayChange() {} },
    document: { activeElement: null, body: {} },
    CSS: { supports: (_field, value) => value !== 'invalid' },
    setTimeout: callback => { const id = ++sequence; timers.set(id, callback); return id; },
    clearTimeout: id => timers.delete(id),
  });
  vm.runInContext(compiled.code, context);
  const snapshot = {
    target: {
      id: 'target-1',
      tag: 'button',
      style_preview: {
        values: {
          width: '100px',
          height: '40px',
          'padding-top': '10px',
          'padding-right': '14px',
          'padding-bottom': '10px',
          'padding-left': '14px',
          'margin-top': '1px',
          'margin-right': '2px',
          'margin-bottom': '3px',
          'margin-left': '4px',
          'letter-spacing': '-1px',
          'word-spacing': '2px',
          'row-gap': '6px',
          'column-gap': '8px',
        },
        ratio_locked: false,
      },
    },
  };
  return {
    render(overrides = {}) {
      slot = 0;
      return context.module.exports.PageAnnotationPopup({ snapshot, command, working: false, viewportRef: {}, ...overrides });
    },
    timers,
    dispose() { cleanups.forEach(cleanup => cleanup?.()); },
    snapshot,
    document: context.document,
    toasts,
  };
}

function find(tree, predicate) {
  if (!tree || typeof tree !== 'object') return undefined;
  if (predicate(tree)) return tree;
  for (const child of tree.props?.children?.flat(Infinity) || []) {
    const found = find(child, predicate);
    if (found) return found;
  }
  return undefined;
}

const input = (tree, label) => find(tree, node => node.type === 'input' && node.props?.['aria-label'] === label);
const tick = () => new Promise(resolve => setImmediate(resolve));
const openEditor = page => {
  const closed = page.render();
  find(closed, node => node.props?.['aria-label'] === '调整').props.onClick();
  return page.render();
};

{
  const calls = [];
  const page = fixture(async action => { calls.push(action); return {ok:true}; });
  const tree = page.render();
  const event = fields => ({key:'Escape', preventDefault() { this.prevented = true; },
    stopPropagation() { this.stopped = true; }, ...fields});
  for (const fields of [{isComposing:true}, {nativeEvent:{key:'Escape', isComposing:true}}]) {
    const composing = event(fields);
    tree.props.onKeyDown(composing);
    assert.deepEqual(calls, [], 'IME Escape must not cancel a draft');
    assert.equal(composing.prevented, undefined);
  }
  const escape = event({});
  tree.props.onKeyDown(escape);
  assert.deepEqual(calls, ['cancel']);
  assert.equal(escape.prevented, true);
  assert.equal(escape.stopped, true);
  page.render({working:true}).props.onKeyDown(event({}));
  assert.deepEqual(calls, ['cancel']);
  page.dispose();
  console.log('[pass] IME Escape preserves the draft while ordinary Escape cancels once and busy Escape cannot cancel');
}

{
  const calls = [];
  let release;
  const page = fixture(async (action, fields) => {
    calls.push({ action, fields });
    if (action === 'preview_style') return new Promise(resolve => { release = resolve; });
    return { ok: true };
  });
  let tree = openEditor(page);
  assert.equal(find(tree, node => node.props?.['aria-label'] === '锁定宽高比例').props['aria-pressed'], false);
  input(tree, '宽度').props.onChange({ target: { value: '200' } });
  assert.equal(page.timers.size, 1);
  assert.equal(calls.length, 0);
  tree = page.render();
  const saving = tree.props.onSubmit({ preventDefault() {} });
  await tick();
  assert.equal(calls[0].action, 'preview_style');
  assert.equal(calls[0].fields.styles.width, '200px');
  assert.equal(calls.length, 1);
  assert.equal(page.timers.size, 0);
  release({ ok: true });
  await saving;
  assert.equal(calls[1].action, 'save');
  assert.equal(calls[1].fields.description, '');
  page.dispose();
  console.log('[pass] trusted style editor starts ratio-off and flushes unblurred style edits before differences-only save');
}

{
  const page = fixture(async () => ({ok:true}));
  const tree = openEditor(page);
  const control = (tagName, visible = true) => ({tagName, getClientRects:() => visible ? [{}] : [],
    getAttribute:() => null, focus() { page.document.activeElement = this; },
    matches:() => tagName === 'TEXTAREA', closest:() => true});
  const first = control('BUTTON');
  const middle = control('TEXTAREA');
  const last = control('BUTTON');
  const hidden = control('INPUT', false);
  const dialog = {querySelectorAll:() => [first, middle, last, hidden],
    contains:target => [first, middle, last, hidden].includes(target),
    focus() { page.document.activeElement = this; }};
  tree.props.ref.current = dialog;
  const key = (fields = {}) => {
    const event = {key:'Tab', preventDefault() { this.prevented = true; }, ...fields};
    tree.props.onKeyDown(event);
    return event;
  };
  for (const [from, shiftKey, expected] of [[last, false, first], [first, true, last],
    [page.document.body, false, first], [page.document.body, true, last]]) {
    page.document.activeElement = from;
    assert.equal(key({shiftKey}).prevented, true);
    assert.equal(page.document.activeElement, expected);
  }
  page.document.activeElement = middle;
  assert.equal(key().prevented, undefined);
  assert.equal(page.document.activeElement, middle);
  assert.equal(key({key:'Enter', target:first, repeat:true}).prevented, true);
  assert.equal(key({key:'Enter', target:middle, repeat:true}).prevented, undefined);
  assert.equal(key({key:'Enter', target:first, repeat:true, nativeEvent:{isComposing:true}}).prevented, undefined);
  assert.equal(key({key:'Enter', target:middle}).prevented, undefined);
  dialog.querySelectorAll = () => [];
  assert.equal(key().prevented, true);
  assert.equal(page.document.activeElement, dialog);
  assert.equal(tree.props.tabIndex, -1);
  assert.equal(find(tree, node => node.props?.['aria-label'] === '调整').props.type, 'button');
  let blurs = 0;
  const styleEnter = {key:'Enter', nativeEvent:{isComposing:true}, currentTarget:{blur() { ++blurs; }},
    preventDefault() { this.prevented = true; }};
  input(tree, '宽度').props.onKeyDown(styleEnter);
  assert.equal(blurs, 0, 'IME Enter must not blur the style editor');
  assert.equal(styleEnter.prevented, undefined);
  styleEnter.nativeEvent.isComposing = false;
  input(tree, '宽度').props.onKeyDown(styleEnter);
  assert.equal(blurs, 1);
  assert.equal(styleEnter.prevented, true);
  page.dispose();
  console.log('[pass] popup shares dialog Tab wrapping/recapture and Enter repeat/multiline/composition behavior');
}

{
  const page = fixture(async () => ({ok:true}));
  const submit = tree => find(tree, node => node.type === 'button' && node.props.type === 'submit');
  let tree = page.render();
  assert.equal(submit(tree).props.disabled, true, 'empty annotation cannot be submitted');
  const comment = find(tree, node => node.type === 'textarea' && node.props['aria-label'] === '批注描述');
  comment.props.onChange({ target: { value: '这里加粗' } });
  tree = page.render();
  assert.equal(submit(tree).props.disabled, false, 'typed annotation enables submit');
  page.dispose();
  console.log('[pass] annotation submit stays disabled until the comment or a diff exists');
}

{
  const page = fixture(async () => ({ok:true}));
  let tree = page.render();
  let submitted = 0;
  tree.props.ref.current = { requestSubmit() { ++submitted; } };
  const comment = tree => find(tree, node => node.type === 'textarea' && node.props['aria-label'] === '批注描述');
  const key = fields => {
    const event = { key: 'Enter', preventDefault() { this.prevented = true; }, ...fields };
    comment(tree).props.onKeyDown(event);
    return event;
  };
  assert.equal(key({}).prevented, true);
  assert.equal(submitted, 0, 'an empty annotation cannot be added');
  comment(tree).props.onChange({ target: { value: '这里加粗' } });
  tree = page.render();
  assert.equal(key({}).prevented, true);
  assert.equal(submitted, 1, 'plain Enter adds the annotation');
  assert.equal(key({ shiftKey: true }).prevented, undefined);
  assert.equal(submitted, 1, 'Shift+Enter keeps the native newline');
  assert.equal(key({ repeat: true }).prevented, undefined);
  assert.equal(submitted, 1, 'holding Enter cannot add the same annotation repeatedly');
  assert.equal(key({ isComposing: true }).prevented, undefined);
  assert.equal(submitted, 1, 'IME Enter stays with the input method');
  const ctrlEnter = { key: 'Enter', ctrlKey: true, target: comment(tree), preventDefault() { this.prevented = true; } };
  tree.props.onKeyDown(ctrlEnter);
  assert.equal(ctrlEnter.prevented, true);
  assert.equal(submitted, 2, 'Ctrl+Enter saves through the same submit path before sending');
  page.dispose();
  console.log('[pass] comment box Enter adds, Shift+Enter wraps and Ctrl+Enter saves then sends');
}

{
  const cleanups = [];
  const savedSnapshot = { revision: 9, records: [{ id: 'record-9', number: 9 }] };
  const page = fixture(async () => ({ok:true, annotations: savedSnapshot}));
  const sessionId = 'popup-send-failure-session';
  const order = [];
  const onAnnotationsSaved = (next) => {
    assert.deepEqual(next, savedSnapshot);
    order.push('saved');
  };
  cleanups.push(annotationSubmission.registerAnnotationSubmitter(
    sessionId,
    async () => {
      order.push('submit');
      return { ok: false, error: 'offline' };
    },
  ));
  let tree = page.render({
    sessionId,
    onAnnotationsSaved,
  });
  let submitted = 0;
  tree.props.ref.current = { requestSubmit() { ++submitted; } };
  const comment = () => find(tree, node => node.type === 'textarea' && node.props['aria-label'] === '批注描述');
  comment().props.onChange({ target: { value: '这里加粗' } });
  tree = page.render({ sessionId, onAnnotationsSaved });
  tree.props.onKeyDown({ key: 'Enter', ctrlKey: true, target: comment(), preventDefault() {} });
  assert.equal(submitted, 1, 'Ctrl+Enter requests the shared save-and-send path');
  await tree.props.onSubmit({ preventDefault() {} });
  await tick();
  assert.deepEqual(order, ['saved', 'submit'],
    'the saved record reaches the session outbox before the shared submit runs');
  assert.equal(page.toasts.at(-1)?.kind, 'err');
  assert.equal(page.toasts.at(-1)?.text, 'offline',
    'a failed shared submission is surfaced instead of silently ignored');
  cleanups.forEach(cleanup => cleanup());
  page.dispose();
  console.log('[pass] popup reports failed annotation sends after Ctrl+Enter');
}

{
  const page = fixture(async () => ({ok:true}));
  const viewport = { getBoundingClientRect: () => ({ left: 0, top: 0, width: 600, height: 400 }) };
  let tree = page.render({ viewportRef: { current: viewport } });
  const popup = {
    style: {},
    getBoundingClientRect: () => ({ left: 100, top: 50, width: 200, height: 150 }),
  };
  tree.props.ref.current = popup;
  find(tree, node => node.props?.['aria-label'] === '调整').props.onClick();
  tree = page.render({ viewportRef: { current: viewport } });
  assert.equal(tree.props.style['--ace-annotation-adjust-width'], '380px');
  const grip = find(tree, node => node.props?.className === 'ace-annotation-design-grip');
  assert.equal(grip.props.title, '拖动批注框');
  const handle = find(tree, node => node.props?.className === 'ace-annotation-design-handle');
  assert.ok(handle, 'the expanded card keeps a drag surface');
  const targetTag = find(tree, node => node.props?.className === 'ace-annotation-target-tag');
  assert.equal(targetTag?.props.children[0], 'button', 'the expanded card restores the target tag label');
  // The tag sits on the drag-handle row so the header stays one line instead of
  // stacking an empty handle row above the tag.
  assert.ok(find(handle, node => node.props?.className === 'ace-annotation-target-tag'),
    'the target tag shares the drag handle row');
  assert.equal(find(tree, node => node.props?.className === 'ace-annotation-design'
    && find(node, child => child.props?.className === 'ace-annotation-target-tag')), undefined,
  'the tag is no longer the first row of the style stack');
  const dimensions = find(tree, node => node.props?.className === 'ace-annotation-dimensions');
  assert.ok(dimensions, 'width and height share one lockable grid');
  assert.equal(find(dimensions, node => node.props?.['aria-label'] === '锁定宽高比例').props.title, '锁定宽高比例');
  // The width/height lock uses the same bracket + 16px badge as the padding and
  // margin axis pairs instead of a lone floating icon.
  const dimensionBracket = find(dimensions, node => node.props?.className === 'ace-annotation-pair-bracket');
  assert.ok(dimensionBracket, 'the width/height lock draws the shared axis bracket');
  assert.equal(dimensionBracket.props.viewBox, '0 0 26 62');
  assert.equal(find(dimensionBracket, node => node.type === 'path').props.d, 'M4 3H23V21M23 41V59H4');
  const dimensionLock = find(dimensions, node => node.props?.className?.startsWith('ace-annotation-pair-lock'));
  assert.ok(dimensionLock, 'the width/height pair keeps the shared lock wrapper');
  assert.doesNotMatch(dimensionLock.props.className, /is-icon-only/,
    'the width/height lock is no longer an icon-only badge');
  assert.equal(find(dimensionLock, node => node.props?.className === 'ace-annotation-pair-lock-button')
    ?.props['aria-pressed'], false, 'the shared lock button drives the ratio lock state');
  for (const [label, value] of [
    ['上内边距', '10px'],
    ['右内边距', '14px'],
    ['下内边距', '10px'],
    ['左内边距', '14px'],
    ['上外边距', '1px'],
    ['右外边距', '2px'],
    ['下外边距', '3px'],
    ['左外边距', '4px'],
  ]) {
    assert.equal(find(tree, node => node.type === 'input' && node.props?.['aria-label'] === label).props.placeholder, value.replace(/px$/, ''));
  }
  let captured = 0;
  let released = 0;
  let prevented = 0;
  const event = (fields = {}) => ({
    button: 0,
    pointerId: 7,
    clientX: 300,
    clientY: 200,
    currentTarget: {
      setPointerCapture(id) { captured = id; },
      hasPointerCapture() { return true; },
      releasePointerCapture(id) { released = id; },
    },
    preventDefault() { prevented += 1; },
    stopPropagation() {},
    ...fields,
  });
  grip.props.onPointerDown(event());
  grip.props.onPointerMove(event({ clientX: 600, clientY: 450 }));
  assert.deepEqual(popup.style, { left: '392px', top: '242px' });
  grip.props.onPointerUp(event());
  assert.equal(captured, 7);
  assert.equal(released, 7);
  assert.equal(prevented, 2);
  const styles = fs.readFileSync(new URL('../styles/globals.css', import.meta.url), 'utf8');
  assert.match(styles, /\.ace-annotation-design-grip\s*\{[^}]*cursor:\s*grab;/s);
  assert.match(styles, /\.ace-annotation-design-grip:active\s*\{[^}]*cursor:\s*grabbing;/s);
  assert.doesNotMatch(styles, /\.ace-annotation-card\s*\{[^}]*clip-path/s);
  assert.match(styles, /\.ace-annotation-cancel\s*\{[^}]*border:\s*1px solid rgba\(var\(--ace-fg-rgb\), 0\.24\)/s);
  assert.match(styles, /\.ace-annotation-cancel:hover:not\(:disabled\)\s*\{[^}]*border-color:[^}]*background:/s);
  assert.match(styles, /\.ace-annotation-reset\s*\{[^}]*border-radius:\s*999px/s);
  assert.match(styles, /\.ace-annotation-value-cell\s*\{[^}]*padding-left:\s*0/s);
  assert.match(styles, /\.ace-annotation-value-cell\.has-swatch,\s*\.ace-annotation-value-cell\.has-reset\s*\{[^}]*padding-left:\s*44px/s);
  assert.match(styles, /\.ace-annotation-value-pair\s+\.ace-annotation-value-cell\.has-reset\s*\{[^}]*padding-left:\s*22px/s);
  assert.match(styles, /\.ace-annotation-card\.is-adjusting\s*\{[^}]*--ace-annotation-value-width:\s*176px[^}]*width:\s*var\(--ace-annotation-adjust-width, min\(380px,/s);
  assert.match(styles, /\.ace-annotation-spacing-values\s*\{[^}]*grid-template-columns:\s*repeat\(auto-fit, minmax\(45px, 1fr\)\)/s);
  assert.match(styles, /\.ace-annotation-value-pair\s*\{[^}]*grid-template-columns:\s*repeat\(2, minmax\(45px, 1fr\)\)/s);
  assert.match(styles, /\.ace-annotation-pair-lock-button\s*\{[^}]*width:\s*16px[^}]*height:\s*16px[^}]*border:\s*1px solid transparent[^}]*background:\s*var\(--ace-surface\)[^}]*box-shadow:/s);
  assert.match(styles, /\.ace-annotation-pair-lock-button:hover\s*\{[^}]*background:[^}]*color:/s);
  assert.doesNotMatch(styles, /\.ace-annotation-pair-lock:has\(\.ace-annotation-pair-lock-button\[aria-pressed="true"\]\) \.ace-annotation-pair-bracket/);
  assert.match(styles, /\.ace-annotation-expand\s*\{[^}]*display:\s*inline-flex[^}]*align-items:\s*center/s);
  assert.match(styles, /\.ace-annotation-expand\s*\{[^}]*padding:\s*0 2px[^}]*padding-inline:\s*2px/s);
  assert.doesNotMatch(styles, /\.ace-annotation-row > \.ace-annotation-expand\s*\{\s*display:\s*block/s);
  assert.match(styles, /\.ace-annotation-card\s*\{[^}]*border:\s*1px solid rgba\(var\(--ace-fg-rgb, 31, 31, 31\), 0\.26\)/s);
  assert.match(styles, /\.ace-annotation-card\s*\{[^}]*box-shadow:\s*0 1px 2px rgba\(0, 0, 0, 0\.04\), 0 10px 24px rgba\(0, 0, 0, 0\.1\)/s);
  assert.doesNotMatch(styles, /\.ace-annotation-card\s*\{[^}]*box-shadow:[^}]*0 0 0 1px/s,
    'the card drops the heavy 1px halo ring');
  assert.match(styles, /\.ace-annotation-toolbar\s*\{[^}]*box-shadow:\s*0 1px 2px rgba\(0, 0, 0, 0\.12\), 0 8px 22px rgba\(0, 0, 0, 0\.24\)/s);
  assert.doesNotMatch(styles, /\.ace-annotation-toolbar\s*\{[^}]*box-shadow:[^}]*0 0 0 1px/s,
    'the dark toolbar keeps no extra halo ring');
  assert.match(styles, /\.ace-annotation-select-menu\s*\{[^}]*box-shadow:\s*0 1px 2px rgba\(0, 0, 0, 0\.04\), 0 8px 20px rgba\(0, 0, 0, 0\.12\)/s);
  assert.doesNotMatch(styles, /\.ace-annotation-pair-lock\.is-icon-only\s*\{/,
    'the icon-only lock variant is gone now that width/height share the axis bracket');
  assert.ok(/data-ace-native-overlay=\{snapshot\.selection_mode \? undefined : 'overlap'\}/.test(
    fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8'),
  ));
  assert.match(
    fs.readFileSync(new URL('../components/PageAnnotations.jsx', import.meta.url), 'utf8'),
    /data-ace-native-overlay-square=\{snapshot\.selection_mode \? undefined : 'true'\}/,
    'the card opts into a rectangular native hole and draws its own antialiased corner',
  );
  assert.match(styles, /\.ace-annotation-card\.is-adjusting \.ace-annotation-editor\s*\{[^}]*overflow-y:\s*auto[^}]*overflow-x:\s*hidden/s);
  assert.match(styles, /\.ace-annotation-card\.is-adjusting \.ace-annotation-editor\s*\{[^}]*max-height:\s*230px/s);
  assert.match(styles, /\.ace-annotation-card\.is-adjusting \.ace-annotation-comment\s*\{[^}]*max-height:\s*84px/s);
  assert.match(styles, /\.ace-annotation-target-tag\s*\{[^}]*font-size:\s*14px/s);
  // Drag handle row: tag on the left, grip on the right, one line.
  assert.match(styles, /\.ace-annotation-design-handle\s*\{[^}]*justify-content:\s*space-between/s);
  assert.match(styles, /\.ace-annotation-target-tag\s*\{[^}]*min-width:\s*0/s);
  // The scrolling editor must not butt the visible-text box against the
  // action bar divider.
  assert.match(styles, /\.ace-annotation-card\.is-adjusting \.ace-annotation-editor\s*\{[^}]*padding:\s*8px 0 2px/s);
  assert.match(styles, /\.ace-annotation-toolbar\s*\{[^}]*transform:\s*translateX\(-50%\)/s);
  assert.doesNotMatch(styles, /\.ace-annotation-toolbar\s*\{[^}]*translateZ\(0\)/s);
  assert.doesNotMatch(styles, /\.ace-annotation-card\s*\{[^}]*translateZ\(0\)/s);
  assert.doesNotMatch(styles, /\.ace-annotation-select-menu\s*\{[^}]*translateZ\(0\)/s);
  assert.match(styles, /\.ace-annotation-card:not\(\.is-adjusting\)\s*\{[^}]*justify-content:\s*center/s);
  assert.match(styles, /\.ace-annotation-card-header\s*\{[^}]*padding:\s*8px 46px 8px 6px/s);
  assert.doesNotMatch(styles, /\.ace-annotation-adjust\s*\{[^}]*position:\s*absolute/s);
  assert.doesNotMatch(styles, /\.ace-annotation-card:not\(\.is-adjusting\) \.ace-annotation-adjust/);
  assert.match(styles, /\.ace-annotation-card\.is-adjusting \.ace-annotation-editor\s*\{[^}]*max-height:\s*230px/s);
  assert.match(styles, /\.ace-annotation-design-grip\s*\{[^}]*cursor:\s*grab;/s);
  assert.doesNotMatch(styles, /\.ace-annotation-design-grip\s*\{[^}]*transform:\s*rotate\(90deg\)/s);
  page.dispose();
  console.log('[pass] annotation popup handle exposes grab/grabbing cursors and drags within viewport bounds');
}

{
  const calls = [];
  const page = fixture(async (action, fields) => { calls.push({ action, fields }); return { ok: true }; });
  let tree = openEditor(page);
  const opacity = () => input(tree, '不透明度');
  assert.equal(opacity().props.type, 'number');
  assert.equal(opacity().props.step, 1);
  assert.equal(opacity().props.min, 0);
  assert.equal(opacity().props.max, 100);
  opacity().props.onChange({ target: { value: '150' } });
  tree = page.render();
  assert.equal(opacity().props.value, '100');
  opacity().props.onChange({ target: { value: '-4' } });
  tree = page.render();
  assert.equal(opacity().props.value, '0');
  opacity().props.onChange({ target: { value: '42' } });
  await page.render().props.onSubmit({ preventDefault() {} });
  await tick();
  assert.equal(calls[0].action, 'preview_style');
  assert.equal(calls[0].fields.styles.opacity, '0.42');
  page.dispose();
  console.log('[pass] opacity edits use a clamped 0-100 percent control while previewing CSS fractions');
}

{
  const page = fixture(async () => ({ok:true}));
  let tree = openEditor(page);
  find(tree, node => node.props?.['aria-label'] === '展开内边距').props.onClick();
  find(tree, node => node.props?.['aria-label'] === '展开外边距').props.onClick();
  tree = page.render();

  const verticalPadding = find(tree, node => node.props?.['aria-label'] === '解锁内边距垂直值');
  const horizontalPadding = find(tree, node => node.props?.['aria-label'] === '解锁内边距水平值');
  assert.equal(verticalPadding?.props['aria-pressed'], true);
  assert.equal(horizontalPadding?.props['aria-pressed'], true);
  assert.equal(find(tree, node => node.props?.['aria-label'] === '锁定外边距垂直值')?.props['aria-pressed'], false);
  assert.equal(find(tree, node => node.props?.['aria-label'] === '锁定外边距水平值')?.props['aria-pressed'], false);

  let top = find(tree, node => node.type === 'input' && node.props?.['aria-label'] === '上内边距');
  top.props.onChange({target:{value:'20'}});
  tree = page.render();
  assert.equal(find(tree, node => node.type === 'input' && node.props?.['aria-label'] === '下内边距').props.value, '20');

  find(tree, node => node.props?.['aria-label'] === '解锁内边距水平值').props.onClick();
  tree = page.render();
  find(tree, node => node.type === 'input' && node.props?.['aria-label'] === '左内边距').props.onChange({target:{value:'9'}});
  tree = page.render();
  assert.equal(find(tree, node => node.type === 'input' && node.props?.['aria-label'] === '右内边距').props.placeholder, '14');

  find(tree, node => node.props?.['aria-label'] === '锁定外边距垂直值').props.onClick();
  tree = page.render();
  assert.equal(find(tree, node => node.type === 'input' && node.props?.['aria-label'] === '下外边距').props.value, '1');
  assert.equal(find(tree, node => node.props?.['aria-label'] === '解锁外边距垂直值').props['aria-pressed'], true);
  page.dispose();
  console.log('[pass] spacing pairs infer, lock, propagate, and unlock vertical/horizontal values independently');
}

{
  const calls = [];
  let release;
  const page = fixture(async (action, fields) => {
    calls.push({ action, fields });
    if (calls.length === 1) return new Promise(resolve => { release = resolve; });
    return { ok: true };
  });
  let tree = openEditor(page);
  input(tree, '宽度').props.onChange({ target: { value: '200' } });
  [...page.timers.values()][0]();
  tree = page.render();
  input(tree, '宽度').props.onChange({ target: { value: '300' } });
  release({ ok: true });
  await tick();
  tree = page.render();
  await tree.props.onSubmit({ preventDefault() {} });
  assert.equal(calls[1].action, 'preview_style');
  assert.equal(calls[1].fields.styles.width, '300px');
  assert.equal(calls[2].action, 'save');
  page.dispose();
  assert.equal(page.timers.size, 0);
  console.log('[pass] live style preview retains edits made during an in-flight command and disposes pending debounce');
}

{
  const calls = [];
  let fail = true;
  const page = fixture(async (action, fields) => {
    calls.push({ action, fields });
    return { ok: action !== 'preview_style' || !fail };
  });
  let tree = openEditor(page);
  input(tree, '宽度').props.onChange({ target: { value: 'invalid' } });
  assert.equal(page.timers.size, 0);
  await tree.props.onSubmit({ preventDefault() {} });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].action, 'preview_style');
  fail = false;
  tree = page.render();
  await tree.props.onSubmit({ preventDefault() {} });
  assert.equal(calls[1].fields.styles.width, 'invalid');
  assert.equal(calls[2].action, 'save');
  tree = page.render({ snapshot: { ...page.snapshot, original: true } });
  assert.equal(find(tree, node => node.type === 'fieldset').props.disabled, true);
  assert.equal(find(tree, node => node.type === 'button' && node.props.type === 'submit').props.disabled, true);
  await tree.props.onSubmit({ preventDefault() {} });
  assert.equal(calls.length, 3);
  page.dispose();
  console.log('[pass] failed previews prevent save and retain retryable editor values; original comparison disables editing/save');
}

{
  const calls = [];
  let release;
  const page = fixture(async action => {
    calls.push(action);
    return new Promise(resolve => { release = resolve; });
  });
  const tree = openEditor(page);
  input(tree, '宽度').props.onChange({ target: { value: '200' } });
  const saving = tree.props.onSubmit({ preventDefault() {} });
  await tick();
  page.dispose();
  release({ ok: true });
  await saving;
  assert.deepEqual(calls, ['preview_style']);
  console.log('[pass] popup disposal during an awaited preview cannot save a replacement page or target');
}

{
  const page = fixture(async () => ({ok:true}));
  let tree = openEditor(page);
  const width = input(tree, '宽度');
  assert.equal(width.props.type, 'number');
  assert.equal(width.props.value, '100');
  assert.equal(find(tree, node => node.props?.className === 'ace-annotation-value-unit').props.children[0], 'px');
  assert.equal(find(tree, node => node.props?.['aria-label'] === '字体选项').props.type, 'button');
  assert.equal(input(tree, '字间距').props.type, 'number');
  assert.equal(input(tree, '字间距').props.value, '-1');
  assert.equal(input(tree, '行间距').props.value, '6');
  assert.equal(input(tree, '列间距').props.value, '8');
  assert.equal(find(tree, node => node.props?.['aria-label'] === '恢复宽度').props.tabIndex, -1);
  width.props.onChange({ target: { value: '200' } });
  tree = page.render();
  const reset = find(tree, node => node.props?.['aria-label'] === '恢复宽度');
  assert.equal(reset.props.tabIndex, 0);
  reset.props.onClick();
  tree = page.render();
  assert.equal(input(tree, '宽度').props.value, '100');
  assert.equal(find(tree, node => node.props?.['aria-label'] === '恢复宽度').props.tabIndex, -1);
  page.dispose();
  console.log('[pass] numeric controls split value and px, expose a font dropdown, split flex gaps, and reset edited values');
}
