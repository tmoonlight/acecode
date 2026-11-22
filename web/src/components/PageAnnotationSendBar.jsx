import { useMemo, useRef, useState, useSyncExternalStore } from 'react';
import { createPortal } from 'react-dom';
import { annotationIsSendable, pageAnnotationBatchSender } from '../lib/pageAnnotationBatches.js';
import { setAnnotationChromeSuspended } from '../lib/annotationChromeSuspension.js';
import { capturePageAnnotationScreenshot } from '../lib/pageAnnotations.js';
import { copyImageToSystemClipboard } from '../lib/systemClipboard.js';
import { Modal } from './Modal.jsx';

export function PageAnnotationSendBar({ pageId, snapshot, sessionId, api, page, onAccepted, command, working }) {
  const sender = useMemo(() => api ? pageAnnotationBatchSender(pageId, api) : null, [pageId, api]);
  const state = useSyncExternalStore(sender?.subscribe || emptySubscribe,
    sender?.getSnapshot || emptySnapshot);
  const barRef = useRef(null);
  const dragRef = useRef(null);
  const [position, setPosition] = useState(null);
  const [tooltip, setTooltip] = useState(null);
  const [captureState, setCaptureState] = useState({ working: false, error: '', message: '' });
  const [confirmDiscard, setConfirmDiscard] = useState(false);
  const pending = (snapshot.records || []).filter((record) => record.status !== 'sent');
  const suspendDialogChrome = (enabled) => {
    setAnnotationChromeSuspended('dialog', enabled, next => {
      void command?.('set_modal_suspended', { enabled: next });
    });
  };
  const closeDiscard = () => {
    setConfirmDiscard(false);
    suspendDialogChrome(false);
  };
  const active = !!snapshot.active;
  // Codex keeps the floating toolbar inside annotation mode only. Element and
  // region picking hides it so page hover outlines stay unobstructed, and
  // leaving annotation mode must not strand a lone send button over the page.
  if (!active || snapshot.selection_mode || snapshot.popup) return null;
  const sendable = pending.filter(annotationIsSendable);
  const count = Number.isFinite(snapshot.pending_count) ? snapshot.pending_count : pending.length;
  const idle = !working;
  const run = (action, fields) => {
    if (!idle) return;
    void command?.(action, fields);
  };
  const showTooltip = (event, label) => {
    const rect = event?.currentTarget?.getBoundingClientRect?.();
    if (!rect) return;
    setTooltip({
      label,
      left: Math.max(56, Math.min(window.innerWidth - 56, rect.left + rect.width / 2)),
      top: Math.max(8, rect.top - 38),
    });
  };
  const hideTooltip = () => setTooltip(null);
  const tooltipProps = label => ({
    onPointerEnter: event => showTooltip(event, label),
    onPointerLeave: hideTooltip,
    onFocus: event => showTooltip(event, label),
    onBlur: hideTooltip,
  });
  const beginDrag = event => {
    if (event.button !== 0) return;
    const bar = barRef.current;
    const container = bar?.offsetParent;
    if (!bar || !container) return;
    const bounds = bar.getBoundingClientRect();
    const frame = container.getBoundingClientRect();
    dragRef.current = {
      pointerId: event.pointerId,
      x: event.clientX,
      y: event.clientY,
      left: bounds.left - frame.left,
      top: bounds.top - frame.top,
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
    setPosition({ left: dragRef.current.left, top: dragRef.current.top });
    event.preventDefault();
  };
  const moveDrag = event => {
    const drag = dragRef.current;
    const bar = barRef.current;
    const container = bar?.offsetParent;
    if (!drag || !bar || !container || event.pointerId !== drag.pointerId) return;
    const maxLeft = Math.max(0, container.clientWidth - bar.offsetWidth);
    const maxTop = Math.max(0, container.clientHeight - bar.offsetHeight);
    setPosition({
      left: Math.min(Math.max(drag.left + event.clientX - drag.x, 0), maxLeft),
      top: Math.min(Math.max(drag.top + event.clientY - drag.y, 0), maxTop),
    });
  };
  const finishDrag = event => {
    const drag = dragRef.current;
    if (!drag || event.pointerId !== drag.pointerId) return;
    dragRef.current = null;
    try { event.currentTarget.releasePointerCapture?.(event.pointerId); } catch {}
  };
  const send = (
    <button type="button" disabled={!sender || !sessionId || state.working || (!sendable.length && state.phase === 'idle')}
      className="ace-annotation-send" aria-label="发送"
      {...tooltipProps('发送')}
      onClick={async () => {
        const result = await sender.send({ snapshot, sessionId, api, page });
        if (result?.ok) onAccepted?.(result.annotations);
      }}>
      {state.working ? '发送中' : state.phase === 'idle' ? '发送' : '重试'}
    </button>
  );
  const capture = async () => {
    if (!idle || captureState.working) return;
    setCaptureState({ working: true, error: '', message: '' });
    try {
      const result = await capturePageAnnotationScreenshot(pageId);
      if (result?.ok !== true) throw new Error(result?.error || '网页截图失败');
      const binary = atob(result.data);
      const bytes = new Uint8Array(binary.length);
      for (let index = 0; index < binary.length; ++index) bytes[index] = binary.charCodeAt(index);
      const blob = new Blob([bytes], { type: result.mimeType || 'image/png' });
      const url = URL.createObjectURL(blob);
      let copied;
      try {
        copied = await copyImageToSystemClipboard(url, { mimeType: blob.type });
      } finally {
        URL.revokeObjectURL(url);
      }
      if (copied?.ok !== true) throw new Error(copied?.error || '无法复制截图');
      setCaptureState({ working: false, error: '', message: '已复制截图' });
      setTimeout(() => setCaptureState(current => current.message ? { ...current, message: '' } : current), 1800);
    } catch (error) {
      setCaptureState({ working: false, error: String(error?.message || error), message: '' });
    }
  };
  const holdOriginal = pressed => {
    if (!idle || !snapshot.document_id) return;
    void command?.('original', { enabled: pressed });
  };
  const pageBackground = typeof snapshot.page_background === 'string' &&
    snapshot.page_background.trim() &&
    CSS.supports('color', snapshot.page_background)
    ? snapshot.page_background
    : undefined;
  return (
    <div ref={barRef} data-ace-native-overlay="overlap" className="ace-annotation-toolbar" role="toolbar" aria-label="正在批注"
      style={{
        ...(position ? { left: `${position.left}px`, top: `${position.top}px`, bottom: 'auto', transform: 'none' } : {}),
        ...(pageBackground ? { '--ace-annotation-page-background': pageBackground } : {}),
      }}>
      <span className="ace-annotation-grip" role="button" tabIndex={0} aria-label="移动工具栏" title="移动工具栏"
        onPointerDown={beginDrag} onPointerMove={moveDrag} onPointerUp={finishDrag} onPointerCancel={finishDrag}>
        <i /><i /><i /><i /><i /><i />
      </span>
      <span className="ace-annotation-count" aria-live="polite">{`正在批注 · ${count}`}</span>
      <button type="button" aria-label="截取屏幕截图" disabled={working || captureState.working}
        {...tooltipProps('截取屏幕截图')} onClick={() => { void capture(); }}>
        <ScreenshotIcon />
      </button>
      <button type="button" aria-label="隐藏标记" aria-pressed={!!snapshot.markers_hidden}
        disabled={working} {...tooltipProps('隐藏标记')}
        onClick={() => run('hide_markers', { enabled: !snapshot.markers_hidden })}>
        <EyeSlashIcon />
      </button>
      <button type="button" aria-label="按住查看原始页面" aria-pressed={!!snapshot.original}
        disabled={!snapshot.document_id || working} {...tooltipProps('按住查看原始页面')}
        onPointerDown={event => { event.preventDefault(); holdOriginal(true); }}
        onPointerUp={() => holdOriginal(false)}
        onPointerCancel={() => holdOriginal(false)}
        onPointerLeave={() => { if (snapshot.original) holdOriginal(false); }}
        onKeyDown={event => {
          if ((event.key === ' ' || event.key === 'Enter') && !event.repeat) {
            event.preventDefault();
            holdOriginal(true);
          }
        }}
        onKeyUp={event => {
          if (event.key === ' ' || event.key === 'Enter') holdOriginal(false);
        }}>
        <OriginalIcon />
      </button>
      <button type="button" aria-label="丢弃批注" disabled={working || (!pending.length && !snapshot.popup)}
        {...tooltipProps('丢弃批注')} onClick={() => {
          setConfirmDiscard(true);
          suspendDialogChrome(true);
        }}>
        <TrashIcon />
      </button>
      {/* Annotations are delivered into the current conversation, so with no
          conversation open the send button has nowhere to go. Say so up front
          instead of failing the click with an error. */}
      {!sessionId && <span className="ace-annotation-toolbar-note">请先选择对话</span>}
      {state.error && <span role="alert" className="ace-annotation-toolbar-error">{state.error}</span>}
      {captureState.error && <span role="alert" className="ace-annotation-toolbar-error">{captureState.error}</span>}
      {captureState.message && <span className="ace-annotation-toolbar-note" aria-live="polite">{captureState.message}</span>}
      {send}
      <button type="button" aria-label="退出批注模式" disabled={working} {...tooltipProps('退出批注模式')}
        onClick={() => { if (!working) void command?.('exit'); }}>
        <CloseIcon />
      </button>
      {tooltip && createPortal(
        <div role="tooltip" data-ace-native-overlay="overlap" className="ace-annotation-tooltip"
          style={{ left: `${tooltip.left}px`, top: `${tooltip.top}px` }}>
          {tooltip.label}
        </div>,
        document.body,
      )}
      {confirmDiscard && (
        <Modal width={360} dismissOnBackdrop={false} nativeOverlayMode="overlap"
          nativeOverlayInteraction="disabled" onClose={closeDiscard}>
          <div className="p-5">
            <h2 className="text-[15px] font-semibold text-fg">丢弃所有待处理标注?</h2>
            <p className="mt-2 text-[13px] leading-5 text-fg-2">页面上的所有编辑都将被移除</p>
            <p className="text-[13px] leading-5 text-fg-2">放弃后所有标记和修改都被清空</p>
            <div className="mt-5 flex justify-end gap-2">
              <button type="button"
                className="h-8 rounded-lg border border-border px-3 text-[13px] text-fg-2 hover:bg-surface-hi"
                onClick={closeDiscard}>
                取消
              </button>
              <button type="button"
                className="h-8 rounded-lg bg-red-600 px-3 text-[13px] font-medium text-white hover:bg-red-500 disabled:opacity-50"
                disabled={working}
                onClick={() => {
                  closeDiscard();
                  run('discard');
                }}>
                放弃
              </button>
            </div>
          </div>
        </Modal>
      )}
    </div>
  );
}

function ScreenshotIcon() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
      <circle cx="8" cy="8" r="2.7" fill="none" stroke="currentColor" strokeWidth="1.5" />
      <path d="M8 1.4v2.1M8 12.5v2.1M1.4 8h2.1M12.5 8h2.1" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
    </svg>
  );
}

function EyeSlashIcon() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
      <path d="M2 8.2C3.2 6 5.3 4.6 8 4.6s4.8 1.4 6 3.6c-1.2 2.2-3.3 3.6-6 3.6S3.2 10.4 2 8.2Z" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinejoin="round" />
      <circle cx="8" cy="8.2" r="1.15" fill="currentColor" />
      <path d="M3.2 12.6 12.8 3.6" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
    </svg>
  );
}

function OriginalIcon() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
      <path d="M5.3 3.1H3.6v9.8h1.7M10.7 3.1h1.7v9.8h-1.7" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" />
      <path d="M7.9 5.6 6.2 8l1.7 2.4M8.1 5.6 9.8 8 8.1 10.4" fill="none" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}

function TrashIcon() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
      <path d="M3.4 4.6h9.2M6.3 4.5V3.3h3.4v1.2M4.7 4.6l.45 8.1h5.7l.45-8.1" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinejoin="round" strokeLinecap="round" />
      <path d="M7 6.4v4.6M9 6.4v4.6" fill="none" stroke="currentColor" strokeWidth="1.3" strokeLinecap="round" />
    </svg>
  );
}

function CloseIcon() {
  return (
    <svg viewBox="0 0 16 16" width="16" height="16" aria-hidden="true">
      <path d="M4.5 4.5l7 7M11.5 4.5l-7 7" fill="none" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
    </svg>
  );
}

const emptyState = Object.freeze({ working: false, error: '', phase: 'idle' });
const emptySnapshot = () => emptyState;
const emptySubscribe = () => () => {};
