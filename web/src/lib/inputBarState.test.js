import assert from 'node:assert/strict';
import { getInputBarActionState } from './inputBarState.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('idle 输入使用发送模式', () => {
  const state = getInputBarActionState({ value: 'hello', busy: false });
  assert.equal(state.mode, 'send');
  assert.equal(state.submitLabel, '发送');
  assert.equal(state.submitTitle, '发送 (Enter)');
  assert.equal(state.helperText, 'Enter 发送 · Shift+Enter 换行 · 上下键切换历史消息');
  assert.equal(state.canSubmit, true);
  assert.equal(state.canAbort, false);
});

run('busy 输入使用排队模式并保留中断能力', () => {
  const state = getInputBarActionState({ value: 'next step', busy: true });
  assert.equal(state.mode, 'queue');
  assert.equal(state.submitLabel, '排队');
  assert.equal(state.submitTitle, '排队下一条 (Enter)');
  assert.equal(state.helperText, 'Enter 排队 · Shift+Enter 换行 · 上下键切换历史消息');
  assert.equal(state.canSubmit, true);
  assert.equal(state.canAbort, true);
});

run('busy 空草稿显示停止,空回车不提交', () => {
  for (const value of ['', '   ', '\n\t', '\u3000']) {
    const state = getInputBarActionState({ value, busy: true });
    assert.equal(state.mode, 'stop');
    assert.equal(state.submitLabel, '停止');
    assert.equal(state.submitTitle, '停止当前任务');
    assert.equal(state.canSubmit, false);
    assert.equal(state.canAbort, true);
  }
});

run('busy 附件或上下文草稿显示排队,移除后回到停止', () => {
  const queued = getInputBarActionState({ value: '', busy: true, hasExtras: true });
  assert.equal(queued.mode, 'queue');
  assert.equal(queued.canSubmit, true);
  assert.equal(getInputBarActionState({ value: '', busy: true, hasExtras: false }).mode, 'stop');
});

run('busy 停止模式不被 disabled 或 submitting 变成发送动作', () => {
  for (const blocker of [{ disabled: true }, { submitting: true }]) {
    const state = getInputBarActionState({ value: '', busy: true, ...blocker });
    assert.equal(state.mode, 'stop');
    assert.equal(state.canSubmit, false);
    assert.equal(state.canAbort, true);
  }
});

run('仅空闲且末尾用户消息可重试时允许空输入发送', () => {
  assert.equal(getInputBarActionState({ value: '  ' }).canSubmit, false);
  assert.equal(getInputBarActionState({ value: '  ', canRetryLastUserMessage: true }).canSubmit, true);
  for (const blocker of [{ busy: true }, { disabled: true }, { submitting: true }]) {
    assert.equal(getInputBarActionState({ value: '', canRetryLastUserMessage: true, ...blocker }).canSubmit, false);
  }
  assert.equal(getInputBarActionState({ value: 'next', busy: true, canRetryLastUserMessage: true }).mode, 'queue');
});

run('附件可在空文本时提交', () => {
  const state = getInputBarActionState({ value: '   ', hasExtras: true });
  assert.equal(state.canSubmit, true);
  assert.equal(state.hasExtras, true);
});

run('blocking disabled 状态禁止发送和排队', () => {
  assert.equal(getInputBarActionState({ value: 'hello', busy: false, disabled: true }).canSubmit, false);
  assert.equal(getInputBarActionState({ value: 'hello', busy: true, disabled: true }).canSubmit, false);
});

// 回归:发送请求在途时曾把整个编辑区一起禁用(ChatView 的
// disabled={!!questionForView || composerSubmitting} 一路传到 Slate 的
// readOnly),表现为「输入框突然打不了字、光标进不去,切一下会话又好了」。
// submitting 从此只压提交动作,不参与 disabled。
run('submitting 只压住提交动作,不构成 disabled', () => {
  const state = getInputBarActionState({ value: 'hello', submitting: true });
  assert.equal(state.canSubmit, false, '在途提交期间不允许再发一次');
  assert.equal(state.submitting, true);
  assert.equal(state.mode, 'send', '提交中不改变发送/排队语义');
  assert.equal(state.hasText, true, '文本仍被视为可编辑内容');
});

run('submitting 结束后立即恢复可提交', () => {
  const state = getInputBarActionState({ value: 'hello', submitting: false });
  assert.equal(state.canSubmit, true);
  assert.equal(state.submitting, false);
});

run('busy 排队模式同样受 submitting 约束', () => {
  const state = getInputBarActionState({ value: 'next', busy: true, submitting: true });
  assert.equal(state.mode, 'queue');
  assert.equal(state.canSubmit, false, '入队请求在途时不重复入队');
  assert.equal(state.canAbort, true, '停止按钮永远不被提交状态挡住');
});

// ---- 队列暂停(用户中断回合)下的「继续」模式 ----------------------------
// 触发场景:用户点停止后队列暂停(queuePaused=true),会话 idle,输入框为空。
// 期望行为:发送按钮变成「继续」(mode=resume),可点击;标题 / 帮助文案跟着换,
// 与卡片栈横幅的「继续」同义。中断后末尾用户消息往往同时可重试,但用户此刻
// 看到的是「队列已暂停」横幅,按钮语义必须压过重试。
run('队列暂停 + 空输入 → 发送按钮变「继续」,压过重试', () => {
  const state = getInputBarActionState({ value: '', queuePaused: true, canRetryLastUserMessage: true });
  assert.equal(state.mode, 'resume');
  assert.equal(state.canSubmit, true, '空输入也允许点击,因为这一下是继续而不是发送');
  assert.equal(state.submitLabel, '继续');
  assert.equal(state.submitTitle, '继续发送排队的消息 (Enter)');
  assert.equal(state.helperText, 'Enter 继续发送排队的消息 · Shift+Enter 换行 · 上下键切换历史消息');
  assert.equal(state.canAbort, false);
  assert.equal(getInputBarActionState({ value: '   ', queuePaused: true }).mode, 'resume', '纯空白同样视为空输入');
});

// 触发场景:队列暂停但用户已经在输入框里打了字 / 挂了附件 / 回合又在跑 / 编辑区被禁用。
// 期望行为:有内容就回到普通发送(新消息先走,ChatView 提交时顺带解除暂停);
// busy 时是排队模式;disabled / submitting 仍然压住按钮。
run('队列暂停时只有空输入才是「继续」,其它情况沿用原语义', () => {
  assert.equal(getInputBarActionState({ value: 'hello', queuePaused: true }).mode, 'send');
  assert.equal(getInputBarActionState({ value: 'hello', queuePaused: true }).submitLabel, '发送');
  assert.equal(getInputBarActionState({ value: '', hasExtras: true, queuePaused: true }).mode, 'send', '附件是可发送内容');
  assert.equal(getInputBarActionState({ value: '', busy: true, queuePaused: true }).mode, 'stop', '回合运行中空草稿仍然是停止');
  assert.equal(getInputBarActionState({ value: '', queuePaused: true, disabled: true }).canSubmit, false);
  assert.equal(getInputBarActionState({ value: '', queuePaused: true, submitting: true }).canSubmit, false);
  assert.equal(getInputBarActionState({ value: '', queuePaused: false }).mode, 'send', '未暂停时空输入仍是普通发送(不可点)');
  assert.equal(getInputBarActionState({ value: '', queuePaused: false }).canSubmit, false);
});
