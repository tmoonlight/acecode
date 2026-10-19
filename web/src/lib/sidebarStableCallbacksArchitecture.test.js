// 侧栏回调的结构守卫。
//
// 背景:Desktop 挂机一两天后渲染进程内存耗尽。侧栏里依赖各异的 useCallback 在不同
// 渲染里交替重建,V8 让同一次渲染的闭包共享一个作用域,旧回调把旧渲染的整个作用域
// (含当时的会话列表)留住,一份接一份连成链,随渲染次数无限增长。修法是依赖会变的
// 回调一律用 useStableCallback;这里保证 Sidebar 里剩下的 useCallback 依赖全是稳定的
// (setter、ref、其它稳定回调),身份从第一次渲染起就不再变化,不会接链。

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { parseSync } from '@babel/core';

const source = readFileSync(new URL('../components/Sidebar.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const component = ast.program.body.map((node) => node.declaration || node)
  .find((node) => node.id?.name === 'Sidebar');
assert.ok(component, 'Missing production Sidebar');

const STATE_HOOKS = new Set(['useState', 'useStructuralState', 'usePreference', 'useReducer']);
const calleeName = (init) => (init?.type === 'CallExpression' && init.callee.type === 'Identifier'
  ? init.callee.name : '');

function stableCallbackReport() {
  const stable = new Set();
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
      const name = declaration.id.name;
      if (hook === 'useRef' || hook === 'useStableCallback') {
        stable.add(name);
        if (hook === 'useStableCallback') stableCallbacks += 1;
        continue;
      }
      if (hook !== 'useCallback') continue;
      const deps = declaration.init.arguments[1];
      assert.equal(deps?.type, 'ArrayExpression', `${name} must list its dependencies`);
      const changing = deps.elements
        .map((element) => source.slice(element.start, element.end))
        .filter((dep) => !stable.has(dep) && !/Ref$/.test(dep));
      if (changing.length === 0) stable.add(name);
      else unstable.push(`${name} [${changing.join(', ')}]`);
    }
  }
  return { unstable, stableCallbacks };
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:有人在侧栏里新增或改回 useCallback(fn, [会变的状态或 props])。
// 期望:失败并列出回调名与会变的依赖 —— 应改用 useStableCallback。
run('Sidebar 的 useCallback 只依赖稳定引用', () => {
  const { unstable } = stableCallbackReport();
  assert.deepEqual(unstable, [], `改用 useStableCallback:\n${unstable.join('\n')}`);
});

// 场景:把 useStableCallback 全部撤掉、改回普通 useCallback 并去掉依赖数组。
// 期望:失败 —— 上一条用例只在稳定回调确实存在时才有意义。
run('Sidebar 依赖会变的回调已改用 useStableCallback', () => {
  const { stableCallbacks } = stableCallbackReport();
  assert.ok(stableCallbacks >= 13, `expected the converted callbacks, found ${stableCallbacks}`);
});
