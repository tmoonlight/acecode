// 本文件覆盖「输入框把浏览器光标同步进 Slate 选区」这一修复(用户反馈:光标在行首,
// 用中文输入法打的字却出现在行末,切换会话后恢复)。
//
// 根因:Slate 只在 selectionchange 里把 DOM 光标写回 editor.selection,并且在它认为
// 「正在拖拽 / 正在写选区 / 正在合成」时整段跳过。拖拽标记一旦残留(dragstart 之后没有
// dragend/drop),之后的点击、Home 等都不再更新 editor.selection;而 Chrome 的输入法
// 合成结束时,Slate 是把文字插到 editor.selection(过期的行末),不是用户看到的光标处。
// 修复:在 compositionstart / keydown 消费选区之前,用 composerDomSelectionToAdopt 判断
// 是否需要采纳 DOM 选区。
//
// 场景:
//   1. 回归:Slate 选区停在行末、DOM 光标在行首 → 返回行首的 Slate range
//   2. 两边一致 → 不重复 select
//   3. Slate 还有未渲染的改动(渲染中 / 操作未 flush)→ 不用旧 DOM 覆盖新选区
//   4. 编辑器没有焦点、DOM 选区不在编辑器里、没有选区 → 不采纳
//   5. DOM 选区映射失败(返回 null 或抛错)→ 不采纳
//   6. editor.selection 为空时也采纳 DOM 光标
//   7. 生产接线:compositionstart 先采纳 DOM 光标,再标记合成状态并调用父组件回调
//   8. 生产接线:渲染哨兵与 editor.onChange 包装共同维护「未渲染」标记
import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';
import { composerDomSelectionToAdopt } from './composerDomSelection.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const point = (offset) => ({ path: [0, 0], offset });
const caret = (offset) => ({ anchor: point(offset), focus: point(offset) });

// 鸭子类型的假 DOM:editable 只认识自己和 inner 文本节点;document 的 activeElement /
// getSelection 可按场景替换。
function fixture({
  editorSelection = caret(4),
  domRange = caret(0),
  focused = true,
  selectionInside = true,
  rangeCount = 1,
  operations = [],
  renderPending = false,
} = {}) {
  const inner = { nodeType: 3 };
  const outside = { nodeType: 3 };
  const editable = {
    contains: (node) => node === editable || node === inner,
  };
  const node = selectionInside ? inner : outside;
  const domSelection = { rangeCount, anchorNode: node, focusNode: node };
  editable.ownerDocument = {
    activeElement: focused ? editable : { tagName: 'BUTTON' },
    getSelection: () => domSelection,
  };
  const calls = [];
  return {
    calls,
    args: {
      editor: { selection: editorSelection, operations },
      editableElement: editable,
      renderPending,
      toSlateRange: (selection) => {
        calls.push(selection);
        return typeof domRange === 'function' ? domRange() : domRange;
      },
    },
  };
}

// 场景 1(回归):用户先打了「帮我」,editor.selection 在行末 offset 4;之后点击 / Home
// 把 DOM 光标移到行首,但 Slate 的同步被残留的拖拽标记跳过。
// 期望:返回 DOM 光标对应的行首 range,调用方据此 select。
// 修复前:输入法提交的文字插到行末,用户看到光标在行首、字却出现在末尾。
run('stale Slate selection adopts the caret the user actually sees', () => {
  const test = fixture();
  assert.deepEqual(composerDomSelectionToAdopt(test.args), caret(0));
  assert.equal(test.calls.length, 1, 'DOM 选区只映射一次');
});

// 场景 2:正常情况下 Slate 已同步,两边相同。
// 期望:返回 null,不产生多余的 set_selection(否则每次按键都触发一次渲染)。
run('an already synchronized selection is left untouched', () => {
  const test = fixture({ editorSelection: caret(2), domRange: caret(2) });
  assert.equal(composerDomSelectionToAdopt(test.args), null);
});

// 场景 3:程序刚 select 了新位置(例如方向键跨过标签),onChange 已发出但 React 还没把它
// 写进 DOM(renderPending),或者操作还没 flush(operations 非空)。此时 DOM 仍是旧光标。
// 期望:不采纳,也不去映射 DOM(NODE 映射表此时也是旧的)。
// 若误采纳:刚做完的程序化选区会被旧 DOM 光标撤销。
run('pending renders or unflushed operations keep the programmatic selection', () => {
  for (const options of [{ renderPending: true }, { operations: [{ type: 'set_selection' }] }]) {
    const test = fixture(options);
    assert.equal(composerDomSelectionToAdopt(test.args), null);
    assert.equal(test.calls.length, 0, '未渲染时不应读取 DOM 选区');
  }
});

// 场景 4:焦点在别的控件(按钮、终端)、DOM 选区落在编辑器之外、或者根本没有选区。
// 期望:全部返回 null —— 不能把别处的选区搬进输入框。
run('selections outside a focused editor are ignored', () => {
  for (const options of [{ focused: false }, { selectionInside: false }, { rangeCount: 0 }]) {
    const test = fixture(options);
    assert.equal(composerDomSelectionToAdopt(test.args), null, JSON.stringify(options));
    assert.equal(test.calls.length, 0);
  }
  assert.equal(composerDomSelectionToAdopt({}), null, '缺少参数时安全返回');
});

// 场景 5:DOM 正在被 React 替换等情况下,toSlateRange 映射不出来(null)或直接抛错。
// 期望:返回 null,保留 Slate 原选区,输入不会因为异常中断。
run('unmappable DOM selections fall back to the existing Slate selection', () => {
  assert.equal(composerDomSelectionToAdopt(fixture({ domRange: null }).args), null);
  assert.equal(composerDomSelectionToAdopt(fixture({ domRange: () => { throw new Error('Cannot resolve a Slate point'); } }).args), null);
});

// 场景 6:Slate 选区为空(刚挂载、被 deselect)但编辑器已获得焦点且 DOM 有光标。
// 期望:直接采纳 DOM 光标。
run('a missing Slate selection adopts the DOM caret', () => {
  const test = fixture({ editorSelection: null, domRange: caret(3) });
  assert.deepEqual(composerDomSelectionToAdopt(test.args), caret(3));
});

const composerSource = fs.readFileSync(new URL('../components/RichComposer.jsx', import.meta.url), 'utf8');
const ast = parseSync(composerSource, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const callbacks = new Map();
traverse(ast, {
  VariableDeclarator({ node }) {
    if (node.id.name === 'handleCompositionStart' || node.id.name === 'adoptDomSelection') {
      const callback = node.init.arguments[0];
      callbacks.set(node.id.name, composerSource.slice(callback.start, callback.end));
    }
  },
});

// 场景 7:生产代码的 handleCompositionStart。
// 触发:compositionstart 到达(Slate 的 onCompositionStart 先调我们的回调,再决定是否删除
// 选中片段)。
// 期望:先采纳 DOM 光标,再置 active、清 settling,最后把父组件回调的返回值原样交还 Slate。
// 修复前:没有采纳这一步,合成结束时文字落在过期的 editor.selection。
run('composition start adopts the DOM caret before composition state and parent callbacks', () => {
  assert.ok(callbacks.has('handleCompositionStart'), '缺少生产回调 handleCompositionStart');
  const order = [];
  const compositionStateRef = { current: { active: false, settling: true } };
  const context = vm.createContext({
    compositionStateRef,
    clearCompositionSettleTimer: () => order.push('clear'),
    adoptDomSelection: () => {
      order.push(`adopt:${compositionStateRef.current.active}`);
      return true;
    },
    onCompositionStart: () => {
      order.push(`parent:${compositionStateRef.current.active}`);
      return 'parent-result';
    },
  });
  const handleCompositionStart = vm.runInContext(`(${callbacks.get('handleCompositionStart')})`, context);
  assert.equal(handleCompositionStart({}), 'parent-result');
  assert.deepEqual(order, ['clear', 'adopt:false', 'parent:true']);
  assert.equal(compositionStateRef.current.settling, false);
});

// 场景 8:「未渲染」标记的两端都必须接上:editor.onChange 包装置位,与 Editable 同一次提交里
// 的渲染哨兵清除;adoptDomSelection 把它传给判定函数。缺任一端,修复要么失效要么会用旧
// DOM 覆盖新选区。
run('render sentinel and onChange wrapper maintain the pending-render flag', () => {
  assert.match(composerSource, /instance\.onChange = \(options\) => \{[\s\S]*?renderPendingRef\.current = true;[\s\S]*?onChange\(options\);/);
  assert.match(composerSource, /spellCheck\s*\/>\s*<ComposerRenderSentinel onRendered=\{markEditorRendered\} \/>\s*<\/Slate>/);
  assert.match(composerSource, /function ComposerRenderSentinel\(\{ onRendered \}\) \{\s*useSlate\(\);\s*useLayoutEffect\(\(\) => \{\s*onRendered\(\);/);
  assert.match(callbacks.get('adoptDomSelection') || '', /renderPending: renderPendingRef\.current/);
  assert.match(callbacks.get('adoptDomSelection') || '', /exactMatch: false/);
});
