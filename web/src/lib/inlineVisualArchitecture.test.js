// 对话内可视化(openspec add-inline-visuals)的结构守卫:隔离框的安全边界写在源码结构里,
// 这里防止以后的改动悄悄放宽它,并核对画图规范技能 / 系统提示里写给模型的变量名、类名、
// CDN 清单与隔离框外壳一致(模型照着规范写,名字对不上图就画不出来)。

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import {
  INLINE_VISUAL_CDN_ORIGINS,
  INLINE_VISUAL_TONES,
  INLINE_VISUAL_TOKENS,
  inlineVisualBaseCss,
} from './inlineVisualFrame.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const read = (relative) => readFileSync(fileURLToPath(new URL(relative, import.meta.url)), 'utf8');
const block = read('../components/InlineVisualBlock.jsx');
const frame = read('./inlineVisualFrame.js');
const runtime = read('./inlineVisualRuntime.js');
const skill = read('../../../assets/seed/skills/acecode/inline-visuals/SKILL.md');
const guidance = read('../../../src/engine/prompt/diagram_guidance.cpp');

function withoutComments(source) {
  return source.replace(/\/\*[\s\S]*?\*\//g, '').replace(/^\s*\/\/.*$/gm, '').replace(/\s\/\/ .*$/gm, '');
}

// 触发场景:有人给隔离框加 allow-same-origin(框内代码就能读写本页面与 daemon token)、
// allow-popups / allow-top-navigation / allow-forms,或换成别的 iframe 写法。
// 期望行为:sandbox 恰好是 allow-scripts;srcdoc 只来自 buildInlineVisualSrcdoc;
// 不出现任何放宽隔离的 sandbox 关键字。
run('隔离框 sandbox 恰好是 allow-scripts', () => {
  assert.match(block, /export const INLINE_VISUAL_SANDBOX = 'allow-scripts';/);
  assert.match(block, /sandbox=\{INLINE_VISUAL_SANDBOX\}/);
  assert.equal((block.match(/<iframe\b/g) || []).length, 1);
  assert.match(block, /srcDoc=\{srcdoc\}/);
  assert.match(block, /buildInlineVisualSrcdoc\(/);
  for (const source of [block, frame, runtime].map(withoutComments)) {
    for (const loosened of ['allow-same-origin', 'allow-popups', 'allow-top-navigation', 'allow-forms', 'allow-modals']) {
      assert.equal(source.includes(loosened), false, loosened);
    }
  }
});

// 触发场景:页面上任意脚本(或另一个 iframe)向本页 postMessage 冒充隔离框请求追问 / 打开链接。
// 期望行为:组件只处理 event.source 是自己 iframe 的消息,追问与链接再看本页面的用户激活状态;
// 框内运行时也只认来自外层的消息。
run('只认自己 iframe 发来的消息', () => {
  assert.match(block, /event\.source !== frame\.contentWindow/);
  assert.match(block, /parentUserActive\(frame\)/);
  assert.match(block, /limiterRef\.current\(\)/);
  assert.match(runtime, /event\.source !== win\.parent/);
});

// 触发场景:框内运行时被 toString 拼进隔离框,构建期的文案本地化插件会把中文字符串字面量
// 换成 __acecodeT(...) 调用 —— 框里没有这个函数,整个运行时会在启动时报错。
// 期望行为:运行时与外壳模块(去掉注释后)不含任何中文字符;运行时不 import 任何东西。
run('框内运行时不含中文字面量也不依赖其它模块', () => {
  const han = new RegExp('[\\u3400-\\u9fff]');
  assert.equal(han.test(withoutComments(runtime)), false);
  assert.equal(han.test(withoutComments(frame)), false);
  assert.equal(/^\s*import\s/m.test(runtime), false);
});

// 触发场景:模型按 inline-visuals 技能写 var(--fg-3)、class="tone-amber"、class="t-mid" 等。
// 期望行为:技能里列出的每个变量都在隔离框外壳里定义;每个类都有对应样式规则;
// 预置标记 #arrow / #dot 与 ace 接口(send / openLink / palette / mode / onThemeChange)都存在。
run('画图规范技能里的变量与类名在隔离框里都存在', () => {
  const css = inlineVisualBaseCss();
  for (const token of skill.match(/`--[a-z0-9-]+`/g).map((item) => item.slice(1, -1))) {
    if (token.startsWith('--tone-')) {
      assert.ok(css.includes(`${token}:`), token);
    } else {
      assert.ok(INLINE_VISUAL_TOKENS.includes(token), token);
    }
  }
  for (const tone of INLINE_VISUAL_TONES) assert.ok(skill.includes(`\`tone-${tone}\``), tone);
  for (const name of ['node', 'edge', 'dashed', 't', 't-sm', 't-lg', 't-mono', 't-mid', 'clickable',
    'card', 'grid', 'row', 'stack', 'badge', 'muted', 'small', 'mono']) {
    assert.ok(skill.includes(`\`${name}\``), `skill lists ${name}`);
    assert.ok(new RegExp(`\\.${name.replace('-', '\\-')}[{,:\\s\\[]`).test(css), `css defines .${name}`);
  }
  assert.ok(skill.includes('url(#arrow)') && frame.includes('<marker id="arrow"'));
  assert.ok(skill.includes('url(#dot)') && frame.includes('<marker id="dot"'));
  for (const member of ['send:', 'openLink:', 'palette:', 'mode:', 'onThemeChange:']) {
    assert.ok(runtime.includes(member), member);
  }
  assert.ok(runtime.includes('win.sendPrompt = api.send'));
});

// 触发场景:系统提示的画图说明(engine/prompt/diagram_guidance.cpp)告诉模型能用哪些变量和 CDN。
// 期望行为:说明里点名的变量都在外壳里定义;说明里的 CDN 清单与内容安全策略放行的完全一致。
run('系统提示画图说明与隔离框外壳一致', () => {
  for (const token of guidance.match(/var\((--[a-z0-9-]+)\)/g).map((item) => item.slice(4, -1))) {
    assert.ok(INLINE_VISUAL_TOKENS.includes(token), token);
  }
  for (const origin of INLINE_VISUAL_CDN_ORIGINS) {
    const host = origin.replace('https://', '');
    assert.ok(guidance.includes(host), host);
    assert.ok(skill.includes(host), host);
  }
  assert.ok(guidance.includes('`ace.send(text)`'));
  assert.ok(guidance.includes('"inline-visuals"') || guidance.includes('kInlineVisualsSkillName'));
});
