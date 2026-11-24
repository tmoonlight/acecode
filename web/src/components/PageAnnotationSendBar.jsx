import { useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { localizedAnnotationSendError, requestAnnotationSubmit } from '../lib/annotationSubmission.js';
import { setAnnotationChromeSuspended } from '../lib/annotationChromeSuspension.js';
import { pageAnnotationPendingCount } from '../lib/pageAnnotationCount.js';
import { Modal } from './Modal.jsx';

// 页内工具栏。发送按钮不自己拼载荷、不自己 POST——它触发输入框那一处的
// submitWithAnnotations（Q35）。工具栏显示当前页的待发条数；会话总数只在
// 输入框上方的待发卡片里展示。
export function PageAnnotationSendBar({
  pageId, snapshot, sessionId, command, working,
}) {
  const barRef = useRef(null);
  const dragRef = useRef(null);
  const [position, setPosition] = useState(null);
  const [tooltip, setTooltip] = useState(null);
  const [sendState, setSendState] = useState({ working: false, error: '' });
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
  // 浮动工具栏只在批注模式里存在：选区（元素/区域）时隐藏，避免挡住 hover 轮廓；
  // 退出批注模式时也不能留下一个孤零零的发送按钮。
  if (!active || snapshot.selection_mode || snapshot.popup) return null;
  const count = pageAnnotationPendingCount(snapshot);
  const idle = !working && !sendState.working;
  const run = (action, fields) => {
    if (working) return;
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
  const send = async () => {
    if (!idle || !sessionId) return;
    setSendState({ working: true, error: '' });
    try {
      const result = await requestAnnotationSubmit(sessionId);
      if (result?.ok === false) {
        setSendState({ working: false, error: localizedAnnotationSendError(result.error) });
        return;
      }
      setSendState({ working: false, error: '' });
    } catch (error) {
      setSendState({ working: false, error: localizedAnnotationSendError(String(error?.message || error)) });
    }
  };
  // 「按住看原图」只有指针一种方式（Q2）：不注册 keydown/keyup，也不让 Enter
  // 触发按钮的默认激活，否则键盘会变成第二条隐形容器。
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
    <div ref={barRef} data-ace-native-overlay="overlap" data-ace-native-overlay-square="true"
      className="ace-annotation-toolbar" role="toolbar" aria-label="正在批注"
      style={{
        ...(position ? { left: `${position.left}px`, top: `${position.top}px`, bottom: 'auto', transform: 'none' } : {}),
        ...(pageBackground ? { '--ace-annotation-page-background': pageBackground } : {}),
      }}>
      <span className="ace-annotation-grip" role="button" tabIndex={0} aria-label="移动工具栏" title="移动工具栏"
        onPointerDown={beginDrag} onPointerMove={moveDrag} onPointerUp={finishDrag} onPointerCancel={finishDrag}>
        <i /><i /><i /><i /><i /><i />
      </span>
      <span className="ace-annotation-count" aria-live="polite">{`正在批注 · ${count}`}</span>
      <button type="button" aria-label="隐藏标记" aria-pressed={!!snapshot.markers_hidden}
        disabled={working} {...tooltipProps('隐藏标记')}
        onClick={() => run('hide_markers', { enabled: !snapshot.markers_hidden })}>
        <EyeSlashIcon />
      </button>
      <button type="button" aria-label="按住查看原始页面" aria-pressed={!!snapshot.original}
        disabled={!snapshot.document_id || working} {...tooltipProps('按住查看原始页面')}
        onKeyDown={event => { if (event.key === 'Enter' || event.key === ' ') event.preventDefault(); }}
        onPointerDown={event => { event.preventDefault(); holdOriginal(true); }}
        onPointerUp={() => holdOriginal(false)}
        onPointerCancel={() => holdOriginal(false)}
        onPointerLeave={() => { if (snapshot.original) holdOriginal(false); }}>
        <OriginalIcon />
      </button>
      <button type="button" aria-label="丢弃批注" disabled={working || (!pending.length && !snapshot.popup)}
        {...tooltipProps('丢弃批注')} onClick={() => {
          setConfirmDiscard(true);
          suspendDialogChrome(true);
        }}>
        <TrashIcon />
      </button>
      {/* 批注发送进当前会话：没有会话时按钮无处可去，先说明而不是点下去才报错。 */}
      {!sessionId && <span className="ace-annotation-toolbar-note">请先选择对话</span>}
      {sendState.error && <span role="alert" className="ace-annotation-toolbar-error">{sendState.error}</span>}
      <button type="button" disabled={!sessionId || !idle || count === 0}
        className="ace-annotation-send" aria-label={count ? `发送 ${count} 条批注` : '发送'}
        {...tooltipProps(count ? `发送 ${count} 条批注` : '发送')}
        onClick={() => { void send(); }}>
        {sendState.working ? '发送中' : count ? `发送 ${count}` : '发送'}
      </button>
      <button type="button" aria-label="退出批注模式" disabled={working} {...tooltipProps('退出批注模式')}
        onClick={() => { if (!working) void command?.('exit'); }}>
        <CloseIcon />
      </button>
      {tooltip && createPortal(
        <div role="tooltip" data-ace-native-overlay="overlap" data-ace-native-overlay-square="true"
          className="ace-annotation-tooltip"
          style={{ left: `${tooltip.left}px`, top: `${tooltip.top}px` }}>
          {tooltip.label}
        </div>,
        document.body,
      )}
      {confirmDiscard && (
        <Modal width={360} dismissOnBackdrop={false} nativeOverlayMode="overlap"
          nativeOverlayInteraction="disabled" onClose={closeDiscard}>
          <div className="p-5">
            <h2 className="text-[15px] font-semibold text-fg">丢弃当前页面的批注?</h2>
            <p className="mt-2 text-[13px] leading-5 text-fg-2">这一页的所有编辑都将被移除</p>
            <p className="text-[13px] leading-5 text-fg-2">其他页签的待发批注不受影响</p>
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
