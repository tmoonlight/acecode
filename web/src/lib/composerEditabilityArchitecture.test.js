// 输入框可编辑性的结构不变量。
//
// 背景(用户反馈的现场 bug):Web/Desktop 上输入框会「突然打不了字、光标进不
// 去」,来回切一次会话又恢复。根因不是焦点也不是遮罩,而是 ChatView 把发送
// 请求在途标记 composerSubmitting 并进了 InputBar 的 disabled,disabled 一路
// 传到 Slate 的 <Editable readOnly>,DOM 上直接变成 contenteditable=false;而
// 唯一的复位点是请求的 .finally() 和会话切换时的 setComposerSubmitting(false)
// —— 所以一次慢往返(实测侧边栏轮询把浏览器 6 条连接占满时排队 400ms+,
// daemon 侧还可能卡在 app_config_mu 或同步 SessionStart hook 上)就会让输入框
// 静默锁死最长 30s(api.js 的默认超时),而切会话正好把它擦掉。
//
// 更糟的是这个只读状态在视觉上完全不可见:样式写的是 Tailwind 的 disabled:
// 变体,而该变体只匹配真正的表单元素,对 contenteditable 的 div 永不生效 ——
// 用户看到的是一个长得完全正常、却打不了字的输入框。
//
// 这里守住三条:提交在途不进 disabled、只读状态可见、发送回执不吞掉用户在
// 等待窗口里写下的下一条。

import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseSync, traverse } from '@babel/core';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

run('提交在途只走 submitting,绝不并进 InputBar 的 disabled', () => {
  const chatView = source('components/ChatView.jsx');
  const openingTags = [];
  traverse(parseSync(chatView, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } }), {
    JSXOpeningElement({ node }) {
      if (node.name?.name === 'InputBar') openingTags.push(chatView.slice(node.start, node.end));
    },
  });
  assert.ok(openingTags.length >= 2);
  const composerInputs = openingTags.join('\n');

  // 会话 composer 与主页 composer 两处都不能再把 *Submitting 并进 disabled。
  assert.doesNotMatch(
    composerInputs,
    /disabled=\{[^}]*composerSubmitting[^}]*\}/,
    'composerSubmitting 进 disabled 会让编辑区在发送期间变成只读',
  );
  assert.doesNotMatch(
    composerInputs,
    /disabled=\{[^}]*homeSubmitting[^}]*\}/,
    'homeSubmitting 进 disabled 会让主页输入框在建会话期间变成只读',
  );

  assert.doesNotMatch(
    composerInputs,
    /disabled=\{[^}]*reasoningSwitching[^}]*\}/,
    'reasoning mutation must disable submission without making the editor read-only',
  );

  // 主页与会话 composer 都在提问挂起时**根本不渲染** —— dock 整体换成
  // 提问框,所以它们不需要 disabled,也不该留「请先回答上方问题」这类旧提示。
  assert.doesNotMatch(chatView, /disabled=\{!!questionForView\}/);
  assert.match(chatView, /<div className="ace-composer-dock"[^>]*>\s*\{questionForView \? \(/);

  assert.doesNotMatch(chatView, /请先回答上方问题/);
  assert.match(chatView, /submitting=\{composerSubmitting \|\| reasoningSwitching\}/);
  assert.match(chatView, /submitting=\{homeSubmitting \|\| reasoningSwitching\}/);
});

run('提问挂起时 composer 整体让位给提问框,不留插话入口', () => {
  const chatView = source('components/ChatView.jsx');

  // 提问框承担提问期间唯一的交互面:提交/取消经 resolveQuestion 回流,反馈
  // 由共享 ToolBlock 按 tool_end 的持久化结果渲染。
  assert.match(chatView, /\{questionForView \? \(\s*<QuestionPicker/);
  assert.match(chatView, /onResolve=\{resolveQuestion\}/);
  assert.doesNotMatch(chatView, /setQuestionFeedback|renderFeedbackAfterQuestion|onFeedback=\{handleQuestionFeedback\}/);


  // 输入区只挂在「没有待答问题」的分支里,提问期间 dock 中不存在 InputBar。
  const dockStart = chatView.indexOf('<div className="ace-composer-dock"');
  assert.ok(dockStart > 0, '未找到会话 composer dock');
  const dock = chatView.slice(dockStart, chatView.indexOf('<SessionContentLoading', dockStart));
  assert.match(dock, /\{questionForView \? \(/);
  assert.ok(dock.includes('<QuestionPicker'), '待答问题应挂在 questionForView 分支里');
  assert.ok(dock.includes('<InputBar'), '输入区应挂在 questionForView 的 else 分支里');

  // 「直接输入 = 取消作答交给 AI 继续」(方案 B)已被方案 A 取代:composer 不再
  // 调用提问插话端点,也不再有插话文案。daemon 端点仍留给 TUI/IM 与排队卡片的
  // 「插话」,转录里的 interjected 标记照旧渲染。
  assert.doesNotMatch(chatView, /api\.interjectQuestion\(targetSid/);
  assert.doesNotMatch(chatView, /placeholder=\{questionForView/);
});

run('排队卡片的插话在提问挂起时仍走提问插话,不打断回合', () => {
  const chatView = source('components/ChatView.jsx');
  const guideFlow = chatView.slice(
    chatView.indexOf('const guideQueued = useStableCallback((queuedId) => {'),
    chatView.indexOf('const executeBuiltinCommand = useCallback('),
  );
  assert.ok(guideFlow.includes('api.interjectQuestion(pendingQuestion.sid, {'));
  assert.ok(guideFlow.includes("if (e?.code !== 'NO_PENDING_QUESTION') throw e;"));
  assert.ok(
    guideFlow.indexOf('api.interjectQuestion(') < guideFlow.indexOf('return interruptNow();'),
    '提问挂起时先插话,退回打断只能在 NO_PENDING_QUESTION 之后',
  );
});

run('InputBar 把 submitting 只接到提交动作上', () => {
  const inputBar = source('components/InputBar.jsx');

  assert.match(inputBar, /disabled, submitting = false,/);
  assert.match(
    inputBar,
    /getInputBarActionState\(\{ value: draftValue, disabled, busy, hasExtras, submitting, canRetryLastUserMessage, queuePaused \}\)/,
  );
  // 回车提交也要挡重复提交,否则去掉 disabled 后连点两次会发两条。
  assert.match(
    inputBar,
    /if \(!currentAction\.canSubmit\) return;/,
  );
  // RichComposer(Slate readOnly 的唯一来源)只能吃 disabled,不能吃 submitting。
  const composerProps = inputBar.match(/<RichComposer[\s\S]*?\/>/)?.[0] || '';
  assert.ok(composerProps, '未找到 RichComposer 挂载点');
  assert.match(composerProps, /disabled=\{disabled\}/);
  assert.doesNotMatch(composerProps, /submitting/);
});

run('只读态在 contenteditable 上必须可见', () => {
  const inputBar = source('components/InputBar.jsx');
  const richComposer = source('components/RichComposer.jsx');

  // Editable 渲染的是 div,Tailwind 的 disabled: 变体对它永远不匹配,只能靠
  // aria-disabled 属性变体;属性本身由 RichComposer 写出。
  assert.match(richComposer, /aria-disabled=\{disabled \? 'true' : undefined\}/);
  assert.match(richComposer, /readOnly=\{disabled\}/);
  assert.match(inputBar, /aria-disabled:opacity-50 aria-disabled:cursor-not-allowed/);

  // 负向后行是必须的:aria-disabled:opacity-50 里也含 disabled:opacity-50 子串。
  const composerProps = inputBar.match(/<RichComposer[\s\S]*?\/>/)?.[0] || '';
  assert.doesNotMatch(
    composerProps,
    /(?<!aria-)disabled:opacity-50/,
    'disabled: 变体对 contenteditable div 无效,会让只读态完全没有视觉反馈',
  );
});

run('发送回执不吞掉等待窗口里写下的下一条', () => {
  const chatView = source('components/ChatView.jsx');

  // 编辑区不再锁,发送成功的 .then() 可能晚于用户写下的新内容;清草稿必须
  // 先比对提交那一刻的原文,对不上就整条放弃。
  assert.match(
    chatView,
    /const clearCurrentSessionDraft = useCallback\(\(\{ expectedText = null, expectedContent = undefined \} = \{\}\) => \{/,
  );
  assert.match(
    chatView,
    /if \(expectedText !== null && composerValueRef\.current !== expectedText\) return false;/,
  );
  assert.match(chatView, /const submittedComposerText = composerValueRef\.current;/);
  assert.match(
    chatView,
    /clearCurrentSessionDraft\(\{ expectedText: submittedComposerText, expectedContent: submittedComposerContent \}\)/,
  );
});
