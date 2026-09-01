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

run('busy 空输入禁用排队但不禁用中断', () => {
  const state = getInputBarActionState({ value: '   ', busy: true });
  assert.equal(state.mode, 'queue');
  assert.equal(state.canSubmit, false);
  assert.equal(state.canAbort, true);
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
