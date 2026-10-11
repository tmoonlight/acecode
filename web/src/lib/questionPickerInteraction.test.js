import assert from 'node:assert/strict';
import { isDismissShortcut } from './shortcutBindings.js';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import { clsx } from './format.js';
import * as helpers from './questionPicker.js';

// Compile the production component and exercise its rendered event handlers.
// Hooks retain state across renders; effects and timers use a deterministic clock.
const source = readFileSync(new URL('../components/QuestionPicker.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const body = ast.program.body.filter((node) => node.type !== 'ImportDeclaration')
  .map((node) => node.declaration || node);
const transformed = await transformWithEsbuild(body.map((node) => source.slice(node.start, node.end)).join('\n'), 'QuestionPicker.jsx', {
  loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment',
});

function harness(questions) {
  const slots = [], sent = [], timers = new Map();
  let cursor = 0, dirty = false, timerId = 0, effects = [], tree;
  let request = { request_id: 'r1', session_id: 's1', questions };
  const changed = (before, after) => !before || after.some((value, index) => value !== before[index]);
  const Component = vm.runInNewContext(`${transformed.code}; QuestionPicker;`, {
    React, clsx, isDismissShortcut, ...helpers, VsIcon: () => null,
    connection: { sendQuestionAnswer: (payload) => sent.push(JSON.parse(JSON.stringify(payload))) },
    window: { getSelection: () => null },
    requestAnimationFrame: (callback) => callback(),
    setTimeout: (callback) => { timers.set(++timerId, callback); return timerId; },
    clearTimeout: (id) => timers.delete(id),
    useCallback: (callback) => callback,
    useState(initial) {
      const index = cursor++;
      if (!(index in slots)) slots[index] = typeof initial === 'function' ? initial() : initial;
      return [slots[index], (next) => {
        const value = typeof next === 'function' ? next(slots[index]) : next;
        if (value !== slots[index]) { slots[index] = value; dirty = true; }
      }];
    },
    useRef(initial) {
      const index = cursor++;
      if (!(index in slots)) slots[index] = { current: initial };
      return slots[index];
    },
    useMemo(factory, deps) {
      const index = cursor++;
      if (changed(slots[index]?.deps, deps)) slots[index] = { deps, value: factory() };
      return slots[index].value;
    },
    useEffect(callback, deps) {
      const index = cursor++;
      if (changed(slots[index]?.deps, deps)) {
        const previous = slots[index];
        slots[index] = { deps };
        effects.push(() => { previous?.cleanup?.(); slots[index].cleanup = callback(); });
      }
    },
  });
  function render(nextRequest) {
    if (nextRequest) request = nextRequest;
    do {
      cursor = 0; dirty = false; effects = [];
      tree = Component({ request });
      effects.forEach((effect) => effect());
    } while (dirty);
    return tree;
  }
  return {
    render, sent, timers,
    unmount: () => slots.forEach((slot) => slot?.cleanup?.()),
  };
}

function nodes(element) {
  return React.isValidElement(element)
    ? [element, ...React.Children.toArray(element.props.children).flatMap(nodes)] : [];
}
const rows = (tree) => nodes(tree).filter((node) => node.props.onMouseDown);
const input = (tree) => nodes(tree).find((node) => node.type === 'input');
const button = (tree, label) => nodes(tree).find((node) => node.type === 'button' && node.props['aria-label'] === label);
function event(overrides = {}) {
  return {
    key: '', detail: 1, target: { tagName: 'SECTION', closest: () => null }, currentTarget: {},
    prevented: false, preventDefault() { this.prevented = true; }, stopPropagation() {}, ...overrides,
  };
}
function key(picker, name, overrides) {
  const e = event({ key: name, ...overrides });
  picker.render().props.onKeyDown(e);
  return e;
}
const q1 = { id: 'q1', text: 'Q1', multiSelect: true, options: [{ label: 'A', value: 'a' }, { label: 'B', value: 'b', recommended: true }] };
const q2 = { id: 'q2', text: 'Q2', options: [{ label: 'C', value: 'c' }] };
function run(name, fn) { fn(); console.log(`[pass] ${name}`); }

run('multi-select double click preserves the clicked answer before advancing', () => {
  const picker = harness([q1, q2]);
  rows(picker.render())[0].props.onClick(event());
  rows(picker.render())[0].props.onMouseDown(event({ detail: 2 }));
  key(picker, 'Enter', { ctrlKey: true });
  assert.deepEqual(picker.sent[0].answers[0].selected, ['a']);
});

run('explicit mouse confirmation preserves an already selected multi-select option', () => {
  const picker = harness([q1, q2]);
  rows(picker.render())[0].props.onClick(event());
  button(rows(picker.render())[0], '选择并进入下一题').props.onClick(event());
  key(picker, 'Enter', { ctrlKey: true });
  assert.deepEqual(picker.sent[0].answers[0].selected, ['a']);
});

run('Enter and number-key confirmation retain an already selected multi-select answer', () => {
  for (const shortcut of ['Enter', '1']) {
    const picker = harness([q1, q2]);
    rows(picker.render())[0].props.onClick(event());
    key(picker, shortcut);
    key(picker, 'Enter', { ctrlKey: true });
    assert.deepEqual(picker.sent[0].answers[0].selected, ['a']);
  }
});

run('double clicking a nested copy button does not advance or answer', () => {
  const picker = harness([q1, q2]);
  rows(picker.render())[0].props.onMouseDown(event({ detail: 2, target: { closest: () => ({ tagName: 'BUTTON' }) } }));
  key(picker, 'Enter', { ctrlKey: true });
  assert.equal(picker.sent.length, 0);
});

run('Enter defaults to the recommended option and the last question waits for Ctrl+Enter', () => {
  const picker = harness([q1]);
  key(picker, 'Enter');
  assert.equal(picker.sent.length, 0);
  key(picker, 'Enter', { ctrlKey: true });
  assert.deepEqual(picker.sent[0].answers[0].selected, ['b']);
});

run('hover and explicit keyboard focus take precedence over the recommendation', () => {
  for (const mode of ['hover', 'keyboard']) {
    const picker = harness([q1]);
    if (mode === 'hover') rows(picker.render())[0].props.onMouseEnter();
    else key(picker, 'ArrowUp');
    key(picker, 'Enter');
    key(picker, 'Enter', { ctrlKey: true });
    assert.deepEqual(picker.sent[0].answers[0].selected, ['a']);
  }
});

run('native button activation and collapsed Enter do not modify hidden answers', () => {
  const picker = harness([q1]);
  const nativeButton = { tagName: 'BUTTON', closest: () => ({ tagName: 'BUTTON' }) };
  assert.equal(key(picker, 'Enter', { target: nativeButton }).prevented, false);
  button(picker.render(), '折叠').props.onClick();
  assert.equal(key(picker, 'Enter', { target: nativeButton }).prevented, false);
  button(picker.render(), '展开').props.onClick();
  key(picker, 'Enter', { ctrlKey: true });
  assert.deepEqual(picker.sent[0].answers[0].selected, []);
});

run('IME confirmation stays in the input; ordinary input Enter retains the submit shortcut', () => {
  for (const composing of [{ isComposing: true }, { nativeEvent: { isComposing: true } }, { keyCode: 229 }]) {
    const picker = harness([q1]);
    input(picker.render()).props.onFocus();
    input(picker.render()).props.onChange({ target: { value: '中文草稿' } });
    assert.equal(key(picker, 'Enter', { ...composing, target: { tagName: 'INPUT' } }).prevented, false);
    assert.equal(picker.sent.length, 0);
    key(picker, 'Enter', { target: { tagName: 'INPUT' } });
    assert.equal(picker.sent[0].answers[0].custom_text, '中文草稿');
  }
});

run('IME Escape does not clear selected answers', () => {
  const picker = harness([q1]);
  rows(picker.render())[0].props.onClick(event());
  key(picker, 'Escape', { isComposing: true });
  key(picker, 'Enter', { ctrlKey: true });
  assert.deepEqual(picker.sent[0].answers[0].selected, ['a']);
});

run('Esc arming is isolated between requests and its timer is cleared on unmount', () => {
  const picker = harness([q1]);
  key(picker, 'Escape');
  assert.equal(picker.timers.size, 1);
  picker.render({ request_id: 'r2', session_id: 's1', questions: [q1] });
  assert.equal(picker.timers.size, 0);
  key(picker, 'Escape');
  assert.equal(picker.sent.length, 0);
  picker.unmount();
  assert.equal(picker.timers.size, 0);
});

run('multi-select custom answer can be unchecked while retaining a muted draft', () => {
  const picker = harness([q1]);
  input(picker.render()).props.onChange({ target: { value: 'retained draft' } });
  button(picker.render(), '取消自定义答案').props.onClick();
  const draftInput = input(picker.render());
  assert.match(draftInput.props.className, /text-fg-mute/);
  assert.equal(draftInput.props.value, 'retained draft');
  key(picker, 'Enter', { ctrlKey: true });
  assert.equal(picker.sent[0].answers[0].custom_text, undefined);
  picker.unmount();
});

run('refocusing a retained multi-select custom draft reactivates it', () => {
  const picker = harness([q1]);
  input(picker.render()).props.onChange({ target: { value: 'retained draft' } });
  button(picker.render(), '取消自定义答案').props.onClick();
  input(picker.render()).props.onFocus();
  key(picker, 'Enter', { ctrlKey: true });
  assert.equal(picker.sent[0].answers[0].custom_text, 'retained draft');
  picker.unmount();
});

run('empty custom input Enter skips locally and only the last question submits', () => {
  const picker = harness([q1, q2]);
  input(picker.render()).props.onFocus();
  key(picker, 'Enter', { target: { tagName: 'INPUT' } });
  assert.equal(picker.sent.length, 0);
  key(picker, 'Enter', { ctrlKey: true });
  assert.equal(picker.sent[0].answers[0].not_answered, true);
});

run('same request re-rendered with a new object reference keeps answers and position', () => {
  const picker = harness([q1, q2]);
  // 双击第一题第一个选项:选中并本地推进到第二题(daemon first-wins,中途不发送)。
  rows(picker.render())[0].props.onMouseDown(event({ detail: 2 }));
  const tabular = (tree) => nodes(tree).find(
    (n) => /tabular-nums/.test(n.props?.className || '') && typeof n.props.children === 'string');
  assert.equal(tabular(picker.render()).props.children, '2 / 2');
  input(picker.render()).props.onChange({ target: { value: 'keep this draft' } });
  // 模拟 App 重渲染:request_id 不变,但传入内容相同、引用全新的 request 对象。
  picker.render({ request_id: 'r1', session_id: 's1', questions: [q1, q2] });
  // 修复前:重置 effect 误把引用变化当作新请求 -> 跳回第一题('1 / 2')并清空答案。
  assert.equal(tabular(picker.render()).props.children, '2 / 2');
  assert.equal(input(picker.render()).props.value, 'keep this draft');
  // 回到第一题,断言已选答案内容仍保留(不只是位置没跳)。
  button(picker.render(), '上一题').props.onClick();
  assert.match(rows(picker.render())[0].props.className, /text-accent/);
});

run('switching to a new request resets answers and position', () => {
  const picker = harness([q1, q2]);
  rows(picker.render())[0].props.onMouseDown(event({ detail: 2 }));
  const tabular = (tree) => nodes(tree).find(
    (n) => /tabular-nums/.test(n.props?.className || '') && typeof n.props.children === 'string');
  assert.equal(tabular(picker.render()).props.children, '2 / 2');
  // 切换到新的提问请求(request_id 变化):必须重置到第一题并清空答案。
  picker.render({ request_id: 'r2', session_id: 's1', questions: [q1, q2] });
  assert.equal(tabular(picker.render()).props.children, '1 / 2');
  assert.doesNotMatch(rows(picker.render())[0].props.className, /text-accent/);
});
