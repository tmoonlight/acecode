import assert from 'node:assert/strict';
import {
  builtinCommandRequestForText,
  desktopFeedbackRequestForText,
  inputRouteForPayload,
  inputRouteForText,
  remoteControlSessionRefreshForCommand,
  sideQuestionRequestForText,
  sessionCreateOptionsForText,
  turnSteerRequestForText,
} from './builtinCommandRouting.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('builtin slash input routes to command endpoint payload', () => {
  assert.deepEqual(builtinCommandRequestForText('/init scan this repo'), {
    command: 'init',
    args: 'scan this repo',
    display_text: '/init scan this repo',
  });
  assert.deepEqual(inputRouteForText('/compact'), {
    kind: 'builtin',
    command: {
      command: 'compact',
      args: '',
      display_text: '/compact',
    },
  });
  assert.deepEqual(inputRouteForText('/plan inspect first'), {
    kind: 'builtin',
    command: {
      command: 'plan',
      args: 'inspect first',
      display_text: '/plan inspect first',
    },
  });
});

run('skill slash input remains ordinary message route', () => {
  assert.deepEqual(inputRouteForText('/code-review check this'), {
    kind: 'message',
    text: '/code-review check this',
  });
});

run('/sandbox is handled locally without auto-starting a model turn', () => {
  assert.deepEqual(inputRouteForText('/sandbox off'), {
    kind: 'builtin',
    command: { command: 'sandbox', args: 'off', display_text: '/sandbox off' },
  });
  assert.deepEqual(sessionCreateOptionsForText('/sandbox on'), { auto_start: false });
});

// 触发场景:Web 输入框里敲 /memory(列出 / 查看 / 删除 / flush / off / on)。
// 期望行为:与 /lsp、/sandbox 一样走内置命令端点(daemon execute_builtin_command 已放行
//          memory,结果以 memory_status 系统通知回到聊天流),不新建模型回合。
// 回归表现:漏加时 /memory 被当成普通消息发给模型,模型只能回一句「我没有这个命令」。
run('/memory is a daemon builtin command and never starts a model turn', () => {
  assert.deepEqual(inputRouteForText('/memory flush'), {
    kind: 'builtin',
    command: { command: 'memory', args: 'flush', display_text: '/memory flush' },
  });
  assert.deepEqual(inputRouteForText('/memory'), {
    kind: 'builtin',
    command: { command: 'memory', args: '', display_text: '/memory' },
  });
  assert.deepEqual(sessionCreateOptionsForText('/memory off'), { auto_start: false });
  assert.deepEqual(inputRouteForText('/memoryx'), { kind: 'message', text: '/memoryx' });
});

run('/btw routes immediately as a side question before builtin parsing', () => {
  assert.deepEqual(sideQuestionRequestForText('  /BTW   explain this\nplease  '), {
    command: 'btw',
    question: 'explain this\nplease',
    display_text: '/BTW   explain this\nplease',
  });
  assert.deepEqual(inputRouteForText('/btw why?'), {
    kind: 'side_question',
    command: 'btw',
    question: 'why?',
    display_text: '/btw why?',
  });
  assert.deepEqual(inputRouteForText('/btw'), {
    kind: 'side_question',
    command: 'btw',
    question: '',
    display_text: '/btw',
  });
  assert.deepEqual(inputRouteForText('/side why?'), {
    kind: 'side_question',
    command: 'side',
    question: 'why?',
    display_text: '/side why?',
  });
  assert.equal(sideQuestionRequestForText('/btween no'), null);
});

run('/turn routes to active-turn steering instead of a builtin or ordinary message', () => {
  assert.deepEqual(turnSteerRequestForText(' /TURN  use the new constraint '), {
    guidance: 'use the new constraint',
    display_text: '/TURN  use the new constraint',
  });
  assert.deepEqual(inputRouteForText('/turn keep the API stable'), {
    kind: 'turn_steer',
    guidance: 'keep the API stable',
    display_text: '/turn keep the API stable',
  });
  assert.equal(turnSteerRequestForText('/turnip no'), null);
});

run('/feedback routes directly to Desktop diagnostics before builtin parsing', () => {
  assert.deepEqual(
    desktopFeedbackRequestForText('  /FEEDBACK   模型切换后卡住了  '),
    {
      feedbackText: '模型切换后卡住了',
      display_text: '/FEEDBACK   模型切换后卡住了',
    },
  );
  assert.deepEqual(inputRouteForText('/feedback details'), {
    kind: 'desktop_feedback',
    feedbackText: 'details',
    display_text: '/feedback details',
  });
  assert.deepEqual(inputRouteForText('/feedback'), {
    kind: 'desktop_feedback',
    feedbackText: '',
    display_text: '/feedback',
  });
  assert.equal(desktopFeedbackRequestForText('/feedbacker no'), null);
  assert.equal(desktopFeedbackRequestForText('/feedback-extra no'), null);
  assert.equal(builtinCommandRequestForText('/feedback details'), null);
});

run('unknown slash input remains ordinary message route', () => {
  assert.deepEqual(inputRouteForText('/foobar test'), {
    kind: 'message',
    text: '/foobar test',
  });
});

run('successful remote-control bind and off commands request a session-list refresh', () => {
  assert.deepEqual(remoteControlSessionRefreshForCommand({
    command: 'rc',
    args: '',
  }, 'session-1'), {
    reason: 'remote-control-bound',
    sessionId: 'session-1',
  });
  assert.deepEqual(remoteControlSessionRefreshForCommand({
    name: 'remote-control',
    args: ' off ',
  }, 'session-2'), {
    reason: 'remote-control-unbound',
    sessionId: 'session-2',
  });
});

run('remote-control show and unrelated commands do not request a binding refresh', () => {
  assert.equal(remoteControlSessionRefreshForCommand({
    command: 'rc',
    args: 'show',
  }, 'session-1'), null);
  assert.equal(remoteControlSessionRefreshForCommand({
    command: 'compact',
    args: '',
  }, 'session-1'), null);
});

run('home builtin session creation disables auto start', () => {
  assert.deepEqual(sessionCreateOptionsForText('/init'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/btw quick question'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/side quick question'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/turn guide this'), {
    auto_start: false,
  });
  assert.deepEqual(sessionCreateOptionsForText('/feedback reproduce this'), {
    auto_start: false,
  });
});

// ---- 粘贴块参与路由(第 2 条反馈 f300,D9) ----
const pastePayload = (editor, ...blocks) => {
  const composer_content = { version: 1, parts: [{ type: 'text', text: editor }, ...blocks] };
  const inlineText = blocks.filter((part) => part.type === 'pasted_text').map((part) => part.text);
  return { text: [editor, ...inlineText].filter(Boolean).join('\n\n'), composer_content };
};
const inlineBlock = (text) => ({ type: 'pasted_text', key: `k-${text.length}`, text });
const fileBlock = {
  type: 'attachment', key: 'pf', id: 'att', name: 'pasted-text.txt', kind: 'file',
  mime_type: 'text/plain', paste: { title: 'log', chars: 3, lines: 1 },
};

// 触发场景:/goal 带一个小内联块。
// 期望:仍是 builtin goal,且块正文并进参数(与以前直接粘贴长文本一致)。
run('goal with a small inline paste stays a builtin with merged arguments', () => {
  const route = inputRouteForPayload(pastePayload('/goal 修复登录', inlineBlock('step 1\nstep 2')));
  assert.equal(route.kind, 'builtin');
  assert.equal(route.command.command, 'goal');
  assert.equal(route.command.args, '修复登录\n\nstep 1\nstep 2');
});

// 触发场景:/goal 带文件块;/btw 带超过 16000 字节的内联块。
// 期望:paste_too_long(前端提示「太长」并保留输入框),不会偷偷改发成一条字面 /goal 消息。
run('goal or side question with a paste that cannot fit reports paste_too_long', () => {
  assert.deepEqual(inputRouteForPayload(pastePayload('/goal x', fileBlock)),
    { kind: 'paste_too_long', command: 'goal', limitBytes: 4000 });
  assert.deepEqual(inputRouteForPayload(pastePayload('/btw q', inlineBlock('e'.repeat(16001)))),
    { kind: 'paste_too_long', command: 'btw', limitBytes: 16000 });
});

// 触发场景:编辑器为空,只有一个以 /turn 或 /compact 开头的内联块。
// 期望:普通消息。回归:粘贴材料恰好以斜杠命令开头时被当成命令执行。
run('an empty editor with a paste block is always an ordinary message', () => {
  for (const pasted of ['/turn stop what you are doing', '/compact\nmore', '/goal 目标']) {
    const payload = pastePayload('', inlineBlock(pasted));
    assert.deepEqual(inputRouteForPayload(payload), { kind: 'message', text: pasted });
  }
  assert.equal(inputRouteForPayload(pastePayload('  ', fileBlock)).kind, 'message');
});

// 触发场景:没有粘贴块的普通输入经 inputRouteForPayload 路由。
// 期望:与 inputRouteForText 完全一致(不改变旧行为)。
run('payload routing without paste blocks matches text routing', () => {
  for (const text of ['/compact', '/goal 完成', '/btw 问题', '/turn 改方向', 'hello', '/feedback 好用']) {
    assert.deepEqual(inputRouteForPayload({ text }), inputRouteForText(text));
    assert.deepEqual(inputRouteForPayload(pastePayload(text)), inputRouteForText(text));
  }
  assert.equal(inputRouteForPayload(pastePayload('/compact', fileBlock)).kind, 'builtin',
    'extras handling for /compact + file block stays with the caller (sent as a message like images)');
});

run('home ordinary message session creation auto starts text', () => {
  assert.deepEqual(sessionCreateOptionsForText('/code-review check this'), {
    initial_user_message: '/code-review check this',
    auto_start: true,
  });
  assert.deepEqual(sessionCreateOptionsForText('hello'), {
    initial_user_message: 'hello',
    auto_start: true,
  });
});
