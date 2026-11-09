import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { sessionTitleActionStates } from './sessionTitleActions.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const HOST = { summaryToggle: true, agentBrowser: true, iosSimulator: true };
const BUTTONS = ['summary', 'trajectory', 'sideChat', 'browser', 'iosSimulator', 'sessionMenu'];

// 场景:点侧栏「新建任务」进入首页,没有任何会话。
// 期望:顶栏按钮组照样全部显示(数量、位置与会话页一致),对会话才有意义的按钮全部禁用;
// 浏览器例外,首页就能打开(页面先归属预留的会话 id)。
// 回归(bug 表现):首页顶栏右侧只剩控制台与右侧面板开关,摘要 / 轨迹 / 侧边聊天 /
// 浏览器 / 会话菜单整组消失,切到会话后才冒出来,顶栏布局在两种页面之间跳动;
// 后来按钮回来了,但浏览器在首页一直是灰的,用户没法先开网页再描述任务。
test('新建任务首页:按钮组全部显示,只有浏览器可用,不显示会话标题', () => {
  const states = sessionTitleActionStates({ sessionId: '', ...HOST });
  assert.equal(states.session, false);
  for (const key of BUTTONS) {
    assert.equal(states[key].visible, true, `${key} 应显示`);
    assert.equal(states[key].enabled, key === 'browser', `${key} 可用状态不对`);
  }
});

// 场景:打开一个普通会话。期望:按钮全部可用。
test('会话页:按钮全部可用', () => {
  const states = sessionTitleActionStates({ sessionId: 's1', ...HOST });
  assert.equal(states.session, true);
  for (const key of BUTTONS) assert.equal(states[key].enabled, true, `${key} 应可用`);
});

// 场景:只读的外部会话(sessionRef.readOnly)。期望:侧边聊天禁用但仍占位,
// 其它按钮可用;以前是整个按钮从顶栏消失。
test('只读会话只禁用侧边聊天', () => {
  const states = sessionTitleActionStates({ sessionId: 's1', readOnly: true, ...HOST });
  assert.deepEqual(states.sideChat, { visible: true, enabled: false });
  assert.equal(states.trajectory.enabled, true);
  assert.equal(states.sessionMenu.enabled, true);
});

// 场景:定时任务页 / 专家组件页(ChatView 的 children)仍带着上一次的会话 ref。
// 期望:它们不是会话视图,按钮禁用、不显示标题,不能对那个旧会话开侧边聊天或会话菜单。
test('定时任务页与专家组件页即使带着会话 id 也按无会话处理', () => {
  const states = sessionTitleActionStates({ sessionId: 's1', auxiliaryPage: true, ...HOST });
  assert.equal(states.session, false);
  for (const key of BUTTONS) assert.equal(states[key].enabled, false, `${key} 应禁用`);
});

// 场景:普通浏览器访问 Web UI(没有 Desktop 原生浏览器)、Windows 宿主(没有 iOS 模拟器)、
// 外层没接摘要面板开关。期望:这三个按钮只按宿主能力决定显隐,与有没有会话无关。
test('浏览器 / iOS 模拟器 / 摘要开关只按宿主能力显隐', () => {
  for (const sessionId of ['', 's1']) {
    const states = sessionTitleActionStates({ sessionId });
    assert.equal(states.summary.visible, false);
    assert.equal(states.browser.visible, false);
    assert.equal(states.iosSimulator.visible, false);
    assert.equal(states.trajectory.visible, true);
    assert.equal(states.sessionMenu.visible, true);
  }
});

// 场景:维护者给顶栏加新按钮。期望:按钮只在 SessionTitleActions 定义一次,ChatView
// 在首页 / 辅助页那一栏和会话面板里渲染的是同一个 sessionTitleBar,不各写一份。
test('顶栏按钮组只有一份,首页与会话页渲染同一个元素', () => {
  const chatView = source('components/ChatView.jsx');
  const actions = source('components/SessionTitleActions.jsx');
  assert.equal(chatView.match(/<SessionTitleBar\b/g)?.length, 1);
  assert.equal(chatView.match(/\{sessionTitleBar\}/g)?.length, 2);
  assert.match(chatView, /\{sessionTitleBar\}\n\s*\{children \|\| homeContent\}/);
  for (const label of ['侧边聊天', '打开浏览器', '会话菜单', '打开 iOS 模拟器']) {
    assert.doesNotMatch(chatView, new RegExp(`title="${label}"`), `${label} 不应在 ChatView 里另写一份`);
    assert.match(actions, new RegExp(`label="${label}"`));
  }
  assert.doesNotMatch(chatView, /name="trajectory"|name="ellipsisVertical"/);
  assert.match(chatView, /showLabel=\{titleActionStates\.session\}/);
});
