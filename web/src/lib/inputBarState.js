export function getInputBarActionState({
  value = '',
  disabled = false,
  busy = false,
  hasExtras = false,
  submitting = false,
} = {}) {
  const hasText = String(value || '').trim().length > 0;
  const hasSubmittableContent = hasText || !!hasExtras;
  const isDisabled = !!disabled;
  const isBusy = !!busy;
  // submitting 只压住「再发一次」这个动作。它绝不能并进 disabled —— disabled
  // 会一路传到 Slate 的 readOnly,把整个编辑区变成 contenteditable=false。
  const isSubmitting = !!submitting;
  return {
    hasText,
    hasExtras: !!hasExtras,
    mode: isBusy ? 'queue' : 'send',
    canSubmit: hasSubmittableContent && !isDisabled && !isSubmitting,
    submitting: isSubmitting,
    canAbort: isBusy,
    submitLabel: isBusy ? '排队' : '发送',
    submitTitle: isBusy ? '排队下一条 (Enter)' : '发送 (Enter)',
    helperText: isBusy
      ? 'Enter 排队 · Shift+Enter 换行 · 上下键切换历史消息'
      : 'Enter 发送 · Shift+Enter 换行 · 上下键切换历史消息',
  };
}
