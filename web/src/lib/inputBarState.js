// 设置 > 常规 > 输入框「后续消息处理方式」(daemon 的 web_ui.busy_send_mode):回复中按 Enter /
// 发送按钮是插话(steer,插进当前回合,模型下一步读到)还是排队(queue,回合结束后再发)。
export const BUSY_SEND_MODES = Object.freeze(['steer', 'queue']);
export const DEFAULT_BUSY_SEND_MODE = 'steer';

export function normalizeBusySendMode(value) {
  return BUSY_SEND_MODES.includes(value) ? value : DEFAULT_BUSY_SEND_MODE;
}

// 回复中的一次发送落到插话还是排队。sendMode 来自输入框:'send' = Enter / 发送按钮,按设置走;
// 'queue' = Ctrl+Enter,做设置的另一种。其余('fork' 等)不经过这里。
export function busySendAction(sendMode, busySendMode) {
  const preferred = normalizeBusySendMode(busySendMode);
  if (sendMode !== 'queue') return preferred;
  return preferred === 'queue' ? 'steer' : 'queue';
}

export function getInputBarActionState({
  value = '',
  disabled = false,
  busy = false,
  hasExtras = false,
  submitting = false,
  canRetryLastUserMessage = false,
  queuePaused = false,
  busySendMode = DEFAULT_BUSY_SEND_MODE,
} = {}) {
  const hasText = String(value || '').trim().length > 0;
  const isDisabled = !!disabled;
  const isBusy = !!busy;
  // 队列暂停(用户中断了回合)且输入框为空:发送按钮变成「继续」,点它 / 按
  // Enter 恢复排队消息的发送。它压过「重发末尾用户消息」—— 中断后两者往往
  // 同时满足,而用户此刻看到的是「队列已暂停」横幅,按钮语义必须与横幅一致。
  const isResume = !isBusy && !!queuePaused && !hasText && !hasExtras;
  const isStop = isBusy && !hasText && !hasExtras;
  const hasSubmittableContent = hasText || !!hasExtras || (!busy && canRetryLastUserMessage);
  // submitting 只压住「再发一次」这个动作。它绝不能并进 disabled —— disabled
  // 会一路传到 Slate 的 readOnly,把整个编辑区变成 contenteditable=false。
  const isSubmitting = !!submitting;
  // 回复中有内容时 Enter / 发送按钮默认是「插话」:插进当前回合,模型下一步读到(steer);
  // 排到回合结束是 Ctrl+Enter,不占按钮。设置里选了排队时两者对调(见 busySendAction)。
  const mode = isResume ? 'resume' : isStop ? 'stop' : isBusy ? 'steer' : 'send';
  return {
    hasText,
    hasExtras: !!hasExtras,
    mode,
    canSubmit: (isResume || hasSubmittableContent) && !isDisabled && !isSubmitting,
    submitting: isSubmitting,
    canAbort: isBusy,
    submitLabel: isResume ? '继续' : isStop ? '停止' : '发送',
    submitTitle: isResume
      ? '继续发送排队的消息 (Enter)'
      : isStop ? '停止当前任务' : '发送 (Enter)',
    helperText: isResume
      ? 'Enter 继续发送排队的消息 · Shift+Enter 换行 · 上下键切换历史消息'
      : isStop
        ? '输入消息可排队 · Shift+Enter 换行 · 上下键切换历史消息'
        : isBusy
          ? (normalizeBusySendMode(busySendMode) === 'queue'
            ? 'Enter 排到回合结束 · Ctrl+Enter 插话 · Shift+Enter 换行'
            : 'Enter 发送 · Ctrl+Enter 排到回合结束 · Shift+Enter 换行')
          : 'Enter 发送 · Shift+Enter 换行 · 上下键切换历史消息',
  };
}
