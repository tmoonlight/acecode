import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { transformWithEsbuild } from 'vite';

const filename = new URL('../components/PageAnnotationEditDialog.jsx', import.meta.url);
const compiled = await transformWithEsbuild(fs.readFileSync(filename, 'utf8'), filename.pathname, {
  loader: 'jsx', format: 'cjs', jsx: 'transform', jsxFactory: 'React.createElement',
});

function fixture() {
  const slots = [];
  let slot = 0;
  const React = {
    createElement: (type, props, ...children) => ({ type, props: { ...props, children } }),
    useRef(value) {
      const index = slot++;
      return slots[index] ??= { current: value };
    },
    useState(value) {
      const index = slot++;
      if (!(index in slots)) slots[index] = value;
      return [slots[index], next => { slots[index] = typeof next === 'function' ? next(slots[index]) : next; }];
    },
    useEffect(effect) {
      slot++;
      effect();
    },
  };
  const context = vm.createContext({
    React,
    module: { exports: {} },
    require: name => name === 'react' ? React : { children: null },
    document: { activeElement: null },
  });
  vm.runInContext(compiled.code, context);
  return {
    render(props) {
      slot = 0;
      return context.module.exports.PageAnnotationEditDialog(props);
    },
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

const textOf = (node) => (node?.props?.children || []).flat(Infinity).join('');
const button = (tree, label) => find(tree, node => (
  node.type === 'button' && textOf(node) === label
));
const tick = () => new Promise(resolve => setImmediate(resolve));

{
  const page = fixture();
  const saves = [];
  const record = {
    id: 'record-1',
    number: 1,
    pageTitle: 'Flowfield',
    pageUrl: 'file:///C:/flowfield.html',
    description: '旧描述',
    element: 'em',
    state: 'orphan',
    targets: [{ locator: 'main em' }],
  };
  let tree = page.render({
    record,
    onCancel() {},
    onDelete() {},
    onSave: async (savedRecord, description) => {
      saves.push([savedRecord.id, description]);
      return true;
    },
  });
  const textarea = find(tree, node => node.type === 'textarea');
  assert.equal(textarea.props.readOnly, undefined, 'the description stays editable');
  assert.equal(textarea.props.disabled, undefined, 'the description is not disabled');
  assert.equal(textarea.props.value, '旧描述');
  assert.match(textOf(find(tree, node => node.type === 'p')), /页面已关闭，只能修改文字/);
  assert.equal(textOf(button(tree, '编辑')), '编辑');
  assert.equal(button(tree, '保存'), undefined);

  textarea.props.onChange({ target: { value: '  新描述  ' } });
  tree = page.render({
    record,
    onCancel() {},
    onDelete() {},
    onSave: async (savedRecord, description) => {
      saves.push([savedRecord.id, description]);
      return true;
    },
  });
  button(tree, '编辑').props.onClick();
  tree = page.render({
    record,
    onCancel() {},
    onDelete() {},
    onSave: async (savedRecord, description) => {
      saves.push([savedRecord.id, description]);
      return true;
    },
  });
  assert.equal(button(tree, '保存')?.props.disabled, false);
  button(tree, '保存').props.onClick();
  await tick();
  tree = page.render({
    record,
    onCancel() {},
    onDelete() {},
    onSave: async (savedRecord, description) => {
      saves.push([savedRecord.id, description]);
      return true;
    },
  });
  assert.deepEqual(saves, [['record-1', '新描述']]);
  assert.equal(textOf(button(tree, '编辑')), '编辑', 'saving flips the button back');
  assert.equal(button(tree, '保存'), undefined);
}

{
  const page = fixture();
  const tree = page.render({
    record: {
      id: 'record-2', number: 2, description: '样式目标已被移除',
      element: 'div', state: 'stale', targets: [{ locator: 'main div' }],
    },
    onCancel() {},
    onDelete() {},
    onSave: async () => true,
  });
  assert.match(textOf(find(tree, node => node.type === 'p')), /目标已失效，只能修改文字/);
}

console.log('[pass] no-tab annotation dialog edits, saves, and toggles back to Edit');
