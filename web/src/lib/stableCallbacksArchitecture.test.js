// 大组件回调的结构守卫(Sidebar / ChatView / App)。
//
// 背景:Desktop 挂机一两天后渲染进程内存耗尽。组件里依赖各异的 useCallback 在不同渲染里
// 交替重建,V8 让同一次渲染的闭包共享一个作用域,旧回调把旧渲染的整个作用域(含当时的会话
// 列表、transcript 条目)留住,一份接一份连成链,随渲染次数无限增长。修法是依赖会变的回调
// 一律用 useStableCallback;这里保证剩下的 useCallback 依赖全是稳定的(setter、ref、取自 ref
// 的值、其它稳定回调),或者在白名单里写明了为什么必须保留。

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { parseSync } from '@babel/core';

const STATE_HOOKS = new Set(['useState', 'useStructuralState', 'usePreference', 'useReducer']);

// 必须保留 useCallback 的回调,值是原因。
const KEEP = {
  Sidebar: {},
  ChatView: {
    // 绑定会话:submit 等异步流程在 await 之后调用它们,必须拿到发起那一刻渲染里的版本;
    // 改成稳定引用会在用户中途切会话时作用到新会话上(ChatView 不随会话重新挂载)。
    persistDraftValue: '离开会话的 effect 清理里保存旧会话草稿,要用旧会话的 api',
    clearCurrentSessionDraft: '提交成功后清草稿,绑定发起时的会话',
    recordInputHistory: '提交后写输入历史,绑定发起时的工作区',
    executeBuiltinCommand: '提交中途执行内置命令,绑定发起时的会话',
    materializeWorkspaceDraftPastes: '提交中途上传草稿里的粘贴块,绑定发起时的工作区',
    sendInputOrBuiltin: '提交中途发送输入,绑定发起时的会话',
    persistMediaFilesToSession: '提交中途上传附件,绑定发起时的会话',
    handleComposerChange: '上传完成后回填输入框,绑定发起时的会话',
    openFilePreview: '文件选择器关闭后打开预览,绑定发起时的会话预览区',
    showBrowserPage: '浏览器页创建完成后显示,绑定发起时的会话预览区',
  },
  App: {
    setStoredUiPrefs: '作为参数交给 usePreviewPresentation',
    applyAppearance: '作为 useRef 的初值',
  },
};
// Three update callbacks now live in the external update controller, outside
// App's render closure. Keep the guard for the remaining App callbacks.
const MIN_STABLE = { Sidebar: 13, ChatView: 100, App: 31 };
const FILES = { Sidebar: '../components/Sidebar.jsx', ChatView: '../components/ChatView.jsx', App: '../App.jsx' };

function report(name) {
  const source = readFileSync(new URL(FILES[name], import.meta.url), 'utf8');
  const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
  const component = ast.program.body.map((node) => node.declaration || node)
    .find((node) => node.id?.name === name);
  assert.ok(component, `Missing production component ${name}`);
  const calleeName = (init) => (init?.type === 'CallExpression' && init.callee.type === 'Identifier'
    ? init.callee.name : '');
  const stable = new Set();
  const callbacks = new Set();
  const unstable = [];
  let stableCallbacks = 0;
  for (const statement of component.body.body) {
    if (statement.type !== 'VariableDeclaration') continue;
    for (const declaration of statement.declarations) {
      const hook = calleeName(declaration.init);
      if (declaration.id.type === 'ArrayPattern' && STATE_HOOKS.has(hook)) {
        const setter = declaration.id.elements[1];
        if (setter?.type === 'Identifier') stable.add(setter.name);
        continue;
      }
      if (declaration.id.type !== 'Identifier') continue;
      const id = declaration.id.name;
      const init = declaration.init;
      if (hook === 'useRef' || hook === 'useStableCallback') {
        stable.add(id);
        if (hook === 'useStableCallback') stableCallbacks += 1;
        continue;
      }
      if (init?.type === 'MemberExpression' && init.property.name === 'current'
        && init.object.type === 'Identifier' && /Ref$/.test(init.object.name)) {
        stable.add(id);
        continue;
      }
      if (hook !== 'useCallback') continue;
      callbacks.add(id);
      const deps = init.arguments[1];
      assert.equal(deps?.type, 'ArrayExpression', `${name}.${id} must list its dependencies`);
      const changing = deps.elements
        .map((element) => source.slice(element.start, element.end))
        .filter((dep) => !stable.has(dep) && !/Ref$/.test(dep));
      if (changing.length === 0) stable.add(id);
      else if (!Object.hasOwn(KEEP[name], id)) unstable.push(`${id} [${changing.join(', ')}]`);
    }
  }
  return { unstable, stableCallbacks, callbacks };
}

function run(label, fn) {
  try {
    fn();
    console.log(`[pass] ${label}`);
  } catch (error) {
    console.error(`[fail] ${label}`);
    throw error;
  }
}

for (const name of Object.keys(FILES)) {
  // 场景:有人新增或改回 useCallback(fn, [会变的状态或 props])。
  // 期望:失败并列出回调名与会变的依赖 —— 应改用 useStableCallback,或在 KEEP 里写明原因。
  run(`${name} 的 useCallback 只依赖稳定引用`, () => {
    const { unstable } = report(name);
    assert.deepEqual(unstable, [], `改用 useStableCallback:\n${unstable.join('\n')}`);
  });

  // 场景:白名单里的回调被删除、改名或已经改成 useStableCallback。
  // 期望:失败,提示清理白名单 —— 过期条目会掩盖之后真正需要审查的回调。
  run(`${name} 的保留名单与源码一致`, () => {
    const { callbacks } = report(name);
    for (const id of Object.keys(KEEP[name])) {
      assert.ok(callbacks.has(id), `${name}.${id} 已不是 useCallback,从 KEEP 里删掉`);
    }
  });

  // 场景:把 useStableCallback 全部撤掉、改回普通 useCallback 并去掉依赖数组。
  // 期望:失败 —— 上面的检查只在改造确实存在时才有意义。
  run(`${name} 依赖会变的回调已改用 useStableCallback`, () => {
    const { stableCallbacks } = report(name);
    assert.ok(stableCallbacks >= MIN_STABLE[name], `expected at least ${MIN_STABLE[name]}, found ${stableCallbacks}`);
  });
}
