// 覆盖 lib/questionFeedback.js:AskUserQuestion 反馈卡必须从已落盘的工具消息
// 元数据派生,从而在会话中持久展示 —— 包括回合结束后 transcript self-heal
// 用新 id 覆写最近一轮的情况(这是「卡片随会话输出完成而消失」的根因)。

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import { createTranscriptState, loadTranscriptHistory, reduceTranscriptEvent } from './sessionTranscript.js';
import * as format from './format.js';
import { compactOneLinePreview } from './compactMessagePreview.js';
import { createdFileSource } from './createdFileSource.js';
import { normalizeAttachmentList } from './messageAttachments.js';
import { fallbackToolSummary } from './toolSummaryFallback.js';
import { shellCommandPresentation } from './shellCommandPresentation.js';
import { canonicalTranscriptTool, presentTool } from './toolPresentation.js';
import {
  questionFeedbackForItem,
  questionFeedbackForTool,
} from './questionFeedback.js';

// 编译真实 QuestionFeedbackCard 用于渲染断言。该组件只依赖 VsIcon(图标),其余
// 全在测试里替身化;data-question-feedback 属性是反馈卡机制的可测试锚点。
const feedbackCardSource = readFileSync(
  new URL('../components/QuestionFeedbackCard.jsx', import.meta.url),
  'utf8',
);
const feedbackCardAst = parseSync(feedbackCardSource, {
  configFile: false,
  babelrc: false,
  parserOpts: { plugins: ['jsx'] },
});
const feedbackCardBody = feedbackCardAst.program.body
  .filter((node) => node.type !== 'ImportDeclaration')
  .map((node) => node.declaration || node);
const feedbackCardTransformed = await transformWithEsbuild(
  feedbackCardBody.map((node) => feedbackCardSource.slice(node.start, node.end)).join('\n'),
  'QuestionFeedbackCard.jsx',
  { loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment' },
);
const { QuestionFeedbackCard } = vm.runInNewContext(
  `${feedbackCardTransformed.code}; ({ QuestionFeedbackCard });`,
  { React, ...React, VsIcon: () => null },
);

// 编译共享 ToolBlock,保证测试经过生产调用路径,不单独拼接反馈卡。
const toolSource = readFileSync(new URL('../components/ToolBlock.jsx', import.meta.url), 'utf8');
const toolAst = parseSync(toolSource, {
  configFile: false,
  babelrc: false,
  parserOpts: { plugins: ['jsx'] },
});
const toolBody = toolAst.program.body
  .filter((node) => node.type !== 'ImportDeclaration')
  .map((node) => node.declaration || node);
const toolTransformed = await transformWithEsbuild(
  toolBody.map((node) => toolSource.slice(node.start, node.end)).join('\n'),
  'ToolBlock.jsx',
  { loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment' },
);
const { ToolBlock } = vm.runInNewContext(`${toolTransformed.code}; ({ ToolBlock });`, {
  React, ...React, ...format,
  compactOneLinePreview, createdFileSource, normalizeAttachmentList,
  fallbackToolSummary, questionFeedbackForTool, QuestionFeedbackCard,
  shellCommandPresentation, canonicalTranscriptTool, presentTool,
  ToolDetailMetadata: () => null,
  useTranslation: () => ({ t: (text) => text }),
  renderMarkdown: () => '',
  VsIcon: () => null,
  ToolSummaryIcon: () => null,
  ActivityLine: ({ label }) => React.createElement('span', { 'data-tool-activity': true }, label),
});

export function renderQuestionToolForTest(item) {
  return renderToStaticMarkup(React.createElement(ToolBlock, {
    entry: item.tool,
    sessionRunning: false,
  }));
}

function lastAskUserQuestionItem(items) {
  return items.findLast((item) => item.kind === 'tool' && item.tool?.tool === 'AskUserQuestion');
}

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function load(messages) {
  return loadTranscriptHistory(createTranscriptState({ title: 's1' }), {
    messages,
    events: [],
  }).state;
}

function askToolMessage(metadata, content = 'User has answered your questions') {
  return {
    id: 't1',
    role: 'tool',
    content,
    tool: 'AskUserQuestion',
    tool_call_id: 'call-ask',
    ts: 2,
    metadata,
  };
}

function userMessage() {
  return { id: 'u1', role: 'user', content: '随机问我', ts: 1 };
}

const SUBMIT_METADATA = {
  tool_success: true,
  ask_user_question_result: {
    items: [
      { question: '你最喜欢的语言?', answer: 'Rust', multi_select: false },
      { question: '目标平台?', answer: 'Windows, Linux', multi_select: true },
    ],
  },
};

const CANCEL_METADATA = {
  tool_success: false,
  ask_user_question_result: { cancelled: true, items: [] },
};

run('提交后可恢复出 tool item,并派生「全部提交完成」卡', () => {
  const state = load([userMessage(), askToolMessage(SUBMIT_METADATA)]);
  const item = lastAskUserQuestionItem(state.items);
  assert.ok(item, 'AskUserQuestion 工具消息必须恢复成 tool item');
  const card = questionFeedbackForItem(item);
  assert.ok(card, '提交后必须能派生反馈卡');
  assert.equal(card.kind, 'submit');
  assert.equal(card.summary.length, 2);
  assert.equal(card.summary[0].question, '你最喜欢的语言?');
  assert.equal(card.summary[0].answer, 'Rust');
  assert.equal(card.summary[0].notAnswered, false);
  assert.equal(card.summary[1].answer, 'Windows, Linux');
  assert.equal(card.summary[1].multiSelect, true, '多选标记必须随卡片一起持久化');
});

run('取消后仍能恢复出 tool item,并派生「已取消全部回答」卡', () => {
  const state = load([
    userMessage(),
    askToolMessage(CANCEL_METADATA, '[Error] User declined to answer questions.'),
  ]);
  const item = lastAskUserQuestionItem(state.items);
  assert.ok(item, '取消的 AskUserQuestion 工具消息必须恢复成 tool item');
  const card = questionFeedbackForItem(item);
  assert.ok(card, '取消后必须能派生反馈卡(会话输出完成后依旧存在)');
  assert.equal(card.kind, 'cancel');
});

run('item id 变化不影响反馈卡(回合结束 self-heal 覆写后不丢失)', () => {
  const state = load([userMessage(), askToolMessage(SUBMIT_METADATA)]);
  const item = lastAskUserQuestionItem(state.items);
  const renamed = { ...item, id: 4242 };
  assert.equal(questionFeedbackForItem(renamed).kind, 'submit');
  const cancelledItem = lastAskUserQuestionItem(
    load([userMessage(), askToolMessage(CANCEL_METADATA)]).items,
  );
  assert.equal(questionFeedbackForItem({ ...cancelledItem, id: 4243 }).kind, 'cancel');
});

run('未作答题在反馈卡上标记 notAnswered', () => {
  const state = load([
    userMessage(),
    askToolMessage({
      tool_success: true,
      ask_user_question_result: {
        items: [{ question: '目标平台?', answer: 'Not answered', multi_select: false }],
      },
    }),
  ]);
  const card = questionFeedbackForItem(lastAskUserQuestionItem(state.items));
  assert.equal(card.summary[0].notAnswered, true);
  assert.equal(card.summary[0].answer, '');
});

run('连续提问时待答工具不复用上一题的提交或取消反馈', () => {
  for (const metadata of [SUBMIT_METADATA, CANCEL_METADATA]) {
    let state = load([userMessage(), askToolMessage(metadata)]);
    state = reduceTranscriptEvent(state, {
      type: 'tool_start',
      payload: { tool: 'AskUserQuestion', tool_call_id: 'call-next' },
      seq: state.lastSeq + 1,
    }).state;
    const pending = lastAskUserQuestionItem(state.items);
    assert.equal(pending.tool.isDone, false);
    assert.equal(questionFeedbackForItem(pending), null);
    assert.doesNotMatch(renderQuestionToolForTest(pending), /data-question-feedback/);
    const previous = state.items.find((item) => item.kind === 'tool');
    assert.equal((renderQuestionToolForTest(previous).match(/data-question-feedback=/g) || []).length, 1);

    state = reduceTranscriptEvent(state, {
      type: 'tool_end',
      payload: {
        tool: 'AskUserQuestion',
        tool_call_id: 'call-next',
        success: true,
        metadata: { ask_user_question_result: { items: [{ question: 'Next question?', answer: 'Next answer' }] } },
      },
      seq: state.lastSeq + 1,
    }).state;
    const html = renderQuestionToolForTest(lastAskUserQuestionItem(state.items));
    assert.equal((html.match(/data-question-feedback="submit"/g) || []).length, 1);
    assert.match(html, /Next question\?/);
    assert.match(html, /Next answer/);
    assert.doesNotMatch(html, /Rust|已取消全部回答/);
  }
});

run('共享工具行默认展开已提交结果,仍保留折叠入口', () => {
  const item = lastAskUserQuestionItem(load([userMessage(), askToolMessage(SUBMIT_METADATA)]).items);
  const html = renderQuestionToolForTest(item);
  assert.equal((html.match(/data-question-feedback="submit"/g) || []).length, 1);
  assert.match(html, /data-ask-user-question-result="true"/);
  assert.match(html, /data-desktop-tool-expanded="true"/);
  assert.match(html, /你最喜欢的语言\?|Rust|目标平台\?|Windows, Linux/);
  assert.match(html, /data-tool-activity/);
  assert.doesNotMatch(html, /全部提交完成|（多选）/);
});

run('切换会话后只渲染新会话自己的问答结果', () => {
  renderQuestionToolForTest(lastAskUserQuestionItem(load([userMessage(), askToolMessage(SUBMIT_METADATA)]).items));
  const pending = reduceTranscriptEvent(createTranscriptState({ title: 'other session' }), {
    type: 'tool_start',
    payload: { tool: 'AskUserQuestion', tool_call_id: 'other-call' },
    seq: 1,
  });
  assert.doesNotMatch(renderQuestionToolForTest(lastAskUserQuestionItem(pending.state.items)), /data-question-feedback|Rust/);
});

run('会话级取最近一次提问对应的那条工具消息', () => {
  const state = load([
    userMessage(),
    askToolMessage(SUBMIT_METADATA),
    { id: 'u2', role: 'user', content: '再来一次', ts: 3 },
    {
      ...askToolMessage(CANCEL_METADATA, '[Error] User declined to answer questions.'),
      id: 't2',
      ts: 4,
    },
  ]);
  const item = lastAskUserQuestionItem(state.items);
  assert.equal(item.messageId, 't2', '应取最后一条提问对应的消息');
  assert.equal(questionFeedbackForItem(item).kind, 'cancel');
});

run('运行中、普通失败和无结构化结果都不出反馈卡', () => {
  const answered = {
    kind: 'tool',
    tool: {
      isDone: true,
      success: true,
      tool: 'AskUserQuestion',
      askUserQuestionResult: { items: [{ question: 'Q?', answer: 'A' }] },
    },
  };
  for (const tool of [
    { ...answered.tool, isDone: false },
    { ...answered.tool, success: false },
    { ...answered.tool, askUserQuestionResult: null },
    { ...answered.tool, askUserQuestionResult: { items: [] } },
  ]) {
    assert.equal(questionFeedbackForItem({ ...answered, tool }), null);
    assert.doesNotMatch(renderQuestionToolForTest({ ...answered, tool }), /data-question-feedback/);
  }
  // 非工具条目一律不派生反馈。
  assert.equal(questionFeedbackForItem({ kind: 'msg', tool: answered.tool }), null);
});

run('工具改名或历史页缺少调用名时结构化结果仍渲染反馈卡', () => {
  for (const tool of ['', 'request_input']) {
    const item = {
      kind: 'tool',
      tool: {
        isDone: true,
        success: false,
        tool,
        askUserQuestionResult: { cancelled: true, items: [] },
      },
    };
    assert.equal(questionFeedbackForItem(item)?.kind, 'cancel');
    assert.match(renderQuestionToolForTest(item), /data-question-feedback="cancel"/);
  }
});

// 触发场景:TUI/IM 通道里 AskUserQuestion 挂起时用户直接输入插话,daemon 落盘
// ask_user_question_result={interjected:true, items:[]} 且 success=true。
// 期望:与显式取消使用同一行内展示文案,但保留 interject 数据标记;
// 历史页重载后(工具改名、无调用名)同样可恢复。
run('插话取消作答复用取消展示,且改名后仍可恢复', () => {
  const item = {
    kind: 'tool',
    tool: {
      isDone: true,
      success: true,
      tool: 'AskUserQuestion',
      askUserQuestionResult: { interjected: true, items: [] },
    },
  };
  assert.equal(questionFeedbackForItem(item)?.kind, 'interject');
  const html = renderQuestionToolForTest(item);
  assert.equal((html.match(/data-question-feedback="interject"/g) || []).length, 1);
  assert.ok(html.includes('用户已取消回答'));
  assert.doesNotMatch(html, /data-question-feedback="cancel"|data-question-feedback="submit"|全部提交完成/);
  for (const tool of ['', 'request_input']) {
    const renamed = { ...item, tool: { ...item.tool, tool } };
    assert.equal(questionFeedbackForItem(renamed)?.kind, 'interject');
  }
  // 显式取消优先于插话标记(两者不会同时落盘,守住判定顺序即可)。
  assert.equal(
    questionFeedbackForItem({
      ...item,
      tool: {
        ...item.tool,
        askUserQuestionResult: { cancelled: true, interjected: true, items: [] },
      },
    })?.kind,
    'cancel',
  );
});
