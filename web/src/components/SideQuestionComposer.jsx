import { useEffect, useRef } from 'react';
import { VsIcon } from './Icon.jsx';

export function SideQuestionComposer({
  open,
  value,
  busy = false,
  onChange,
  onSubmit,
  onClose,
}) {
  const textareaRef = useRef(null);

  useEffect(() => {
    if (!open) return;
    window.requestAnimationFrame(() => textareaRef.current?.focus());
  }, [open]);

  if (!open) return null;
  const canSubmit = !!String(value || '').trim() && !busy;
  return (
    <section className="ace-side-question-composer" aria-label="侧边聊天">
      <div className="ace-side-question-composer-header">
        <span className="ace-side-question-composer-icon">
          <VsIcon name="brain" size={13} />
        </span>
        <div>
          <div className="ace-side-question-composer-title">侧边聊天</div>
          <div className="ace-side-question-composer-hint">基于当前会话回答，不会改变主输入草稿</div>
        </div>
        <button
          type="button"
          className="ace-side-question-composer-close"
          aria-label="关闭侧边聊天输入"
          onClick={onClose}
        >
          <VsIcon name="close" size={12} />
        </button>
      </div>
      <div className="ace-side-question-composer-body">
        <textarea
          ref={textareaRef}
          value={value}
          disabled={busy}
          aria-label="侧边聊天问题"
          placeholder="问一个不打断当前任务的问题…"
          rows={2}
          onChange={(event) => onChange?.(event.target.value)}
          onKeyDown={(event) => {
            if (
              event.key === 'Enter'
              && !event.shiftKey
              && !event.isComposing
              && !event.nativeEvent?.isComposing
              && event.keyCode !== 229
            ) {
              event.preventDefault();
              if (canSubmit) onSubmit?.();
            }
          }}
        />
        <button
          type="button"
          className="ace-side-question-composer-send"
          disabled={!canSubmit}
          onClick={onSubmit}
        >
          <VsIcon name="send" size={13} />
          {busy ? '回答中…' : '发送'}
        </button>
      </div>
    </section>
  );
}

export default SideQuestionComposer;
