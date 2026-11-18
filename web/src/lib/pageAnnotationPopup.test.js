import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { transformWithEsbuild } from 'vite';

const filename = new URL('../components/PageAnnotations.jsx', import.meta.url);
const compiled = await transformWithEsbuild(fs.readFileSync(filename, 'utf8'), filename.pathname, {
  loader: 'jsx', format: 'cjs', jsx: 'transform', jsxFactory: 'React.createElement',
});

function fixture(command) {
  const slots = [];
  const timers = new Map();
  const cleanups = [];
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
    require: name => name === 'react' ? React : { notifyNativeSurfaceOverlayChange() {} },
    document: { activeElement: null, body: {} },
    CSS: { supports: (_field, value) => value !== 'invalid' },
    setTimeout: callback => { const id = ++sequence; timers.set(id, callback); return id; },
    clearTimeout: id => timers.delete(id),
  });
  vm.runInContext(compiled.code, context);
  const snapshot = {
    target: { id: 'target-1', tag: 'button', style_preview: { values: { width: '100px', height: '40px' }, ratio_locked: false } },
  };
  return {
    render(overrides = {}) {
      slot = 0;
      return context.module.exports.PageAnnotationPopup({ snapshot, command, working: false, viewportRef: {}, ...overrides });
    },
    timers,
    dispose() { cleanups.forEach(cleanup => cleanup?.()); },
    snapshot,
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

const input = (tree, field) => find(tree, node => node.type === 'input' && node.props.placeholder === field);
const tick = () => new Promise(resolve => setImmediate(resolve));

{
  const calls = [];
  let release;
  const page = fixture(async (action, fields) => {
    calls.push({ action, fields });
    if (action === 'preview_style') return new Promise(resolve => { release = resolve; });
    return { ok: true };
  });
  let tree = page.render();
  assert.equal(find(tree, node => node.type === 'input' && node.props.type === 'checkbox').props.checked, false);
  input(tree, '100px').props.onChange({ target: { value: '200px' } });
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
  const calls = [];
  let release;
  const page = fixture(async (action, fields) => {
    calls.push({ action, fields });
    if (calls.length === 1) return new Promise(resolve => { release = resolve; });
    return { ok: true };
  });
  let tree = page.render();
  input(tree, '100px').props.onChange({ target: { value: '200px' } });
  [...page.timers.values()][0]();
  tree = page.render();
  input(tree, '100px').props.onChange({ target: { value: '300px' } });
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
  let tree = page.render();
  input(tree, '100px').props.onChange({ target: { value: 'invalid' } });
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
  const tree = page.render();
  input(tree, '100px').props.onChange({ target: { value: '200px' } });
  const saving = tree.props.onSubmit({ preventDefault() {} });
  await tick();
  page.dispose();
  release({ ok: true });
  await saving;
  assert.deepEqual(calls, ['preview_style']);
  console.log('[pass] popup disposal during an awaited preview cannot save a replacement page or target');
}
