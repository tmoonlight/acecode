export function getInputBarActionState({
  value = '',
  disabled = false,
  busy = false,
  hasExtras = false,
  submitting = false,
  canRetryLastUserMessage = false,
  queuePaused = false,
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
  const mode = isResume ? 'resume' : isStop ? 'stop' : isBusy ? 'queue' : 'send';
  return {
    hasText,
    hasExtras: !!hasExtras,
    mode,
    canSubmit: (isResume || hasSubmittableContent) && !isDisabled && !isSubmitting,
    submitting: isSubmitting,
    canAbort: isBusy,
    submitLabel: isResume ? '继续' : isStop ? '停止' : isBusy ? '排队' : '发送',
    submitTitle: isResume
      ? '继续发送排队的消息 (Enter)'
      : isStop ? '停止当前任务' : isBusy ? '排队下一条 (Enter)' : '发送 (Enter)',
    helperText: isResume
      ? 'Enter 继续发送排队的消息 · Shift+Enter 换行 · 上下键切换历史消息'
      : isStop
        ? '输入消息可排队 · Shift+Enter 换行 · 上下键切换历史消息'
        : isBusy
          ? 'Enter 排队 · Shift+Enter 换行 · 上下键切换历史消息'
          : 'Enter 发送 · Shift+Enter 换行 · 上下键切换历史消息',
  };
}
