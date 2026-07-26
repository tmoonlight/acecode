import { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { MAX_SELECTION_ANNOTATION_CHARS } from '../lib/selectionChatContext.js';
import { selectionActionPopoverPosition } from '../lib/selectionActionPopover.js';
import { VsIcon } from './Icon.jsx';

export function SelectionActionPopover({
  snapshot,
  mode = 'actions',
  onQuote,
  onStartAnnotation,
  onSubmitAnnotation,
  onCancel,
}) {
  const rootRef = useRef(null);
  const textareaRef = useRef(null);
  const [draft, setDraft] = useState('');
  const [error, setError] = useState('');
  const [position, setPosition] = useState(null);

  const snapshotKey = snapshot?.key || '';

  useEffect(() => {
    setDraft('');
    setError('');
  }, [snapshotKey]);

  useLayoutEffect(() => {
    if (!snapshot?.rect || !rootRef.current) return;
    const rect = rootRef.current.getBoundingClientRect();
    setPosition(selectionActionPopoverPosition(
      snapshot.rect,
      { width: rect.width, height: rect.height },
      { width: window.innerWidth, height: window.innerHeight },
    ));
  }, [mode, snapshot?.rect, snapshotKey]);

  useEffect(() => {
    if (mode !== 'annotation') return;
    const frame = window.requestAnimationFrame(() => {
      textareaRef.current?.focus();
    });
    return () => window.cancelAnimationFrame(frame);
  }, [mode, snapshotKey]);

  useEffect(() => {
    if (!snapshot) return undefined;
    const handlePointerDown = (event) => {
      if (rootRef.current?.contains(event.target)) return;
      onCancel?.();
    };
    const handleResize = () => onCancel?.();
    const handleScroll = (event) => {
      if (rootRef.current?.contains(event.target)) return;
      onCancel?.();
    };
    document.addEventListener('pointerdown', handlePointerDown, true);
    document.addEventListener('scroll', handleScroll, true);
    window.addEventListener('resize', handleResize);
    return () => {
      document.removeEventListener('pointerdown', handlePointerDown, true);
      document.removeEventListener('scroll', handleScroll, true);
      window.removeEventListener('resize', handleResize);
    };
  }, [onCancel, snapshot]);

  if (!snapshot || typeof document === 'undefined') return null;

  const submit = () => {
    const text = draft.trim();
    if (!text) {
      setError('请输入批注内容');
      textareaRef.current?.focus();
      return;
    }
    onSubmitAnnotation?.(text);
  };

  const popover = (
    <div
      ref={rootRef}
      className="ace-selection-action-popover"
      data-mode={mode}
      data-placement={position?.placement || 'below'}
      style={{
        left: position?.left ?? snapshot.rect.left,
        top: position?.top ?? snapshot.rect.bottom + 8,
        visibility: position ? 'visible' : 'hidden',
      }}
      onMouseDown={(event) => {
        if (mode === 'actions') event.preventDefault();
      }}
    >
      {mode === 'annotation' ? (
        <div className="ace-selection-annotation-editor">
          <textarea
            ref={textareaRef}
            value={draft}
            rows={2}
            maxLength={MAX_SELECTION_ANNOTATION_CHARS}
            placeholder="添加批注..."
            aria-label="批注内容"
            aria-invalid={error ? 'true' : 'false'}
            onChange={(event) => {
              setDraft(event.target.value);
              if (error) setError('');
            }}
            onKeyDown={(event) => {
              if (event.key === 'Escape') {
                event.preventDefault();
                onCancel?.();
              } else if (event.key === 'Enter' && !event.shiftKey) {
                event.preventDefault();
                submit();
              }
            }}
          />
          <div className="ace-selection-annotation-footer">
            <span className={error ? 'is-error' : ''}>
              {error || 'Enter 引用 · Shift+Enter 换行'}
            </span>
            <div className="ace-selection-annotation-actions">
              <button type="button" onClick={onCancel}>取消</button>
              <button type="button" className="is-primary" onClick={submit}>引用</button>
            </div>
          </div>
        </div>
      ) : (
        <div className="ace-selection-action-buttons" role="toolbar" aria-label="选中文本操作">
          <button type="button" onClick={onQuote}>
            <VsIcon name="pin" size={13} />
            <span>引用到聊天</span>
          </button>
          <button type="button" onClick={onStartAnnotation}>
            <VsIcon name="edit" size={13} />
            <span>批注</span>
          </button>
        </div>
      )}
    </div>
  );
  return createPortal(popover, document.body);
}
