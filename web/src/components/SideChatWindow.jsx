import { isDismissShortcut } from '../lib/shortcutBindings.js';
import { memo, useCallback, useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  nativeSurfaceViewportRect,
  notifyNativeSurfaceOverlayChange,
} from '../lib/agentBrowserSurfaceCoordinator.js';
import { codeTextFromCopyButtonTarget, copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { renderMarkdown } from '../lib/markdown.js';
import { sideChatToolVerb, sideChatTurnParts } from '../lib/sideChatController.js';
import {
  anchorSideChatGeometry,
  clampSideChatGeometry,
  createSideChatGeometry,
  moveSideChatGeometry,
  resizeSideChatGeometry,
} from '../lib/sideChatGeometry.js';
import {
  SIDE_CHAT_LAUNCHER_PULSE,
  sideChatMotionPlan,
  usableSideChatLauncherRect,
} from '../lib/sideChatMotion.js';
import { VsIcon } from './Icon.jsx';
import { InputBar } from './InputBar.jsx';
import { AttachmentStrip } from './AttachmentStrip.jsx';
import { OverlayLayerContext } from './OverlayLayerContext.jsx';
import { toast } from './Toast.jsx';

const RESIZE_DIRECTIONS = ['n', 'e', 's', 'w', 'ne', 'se', 'sw', 'nw'];
const isComposing = (event) => event.isComposing || event.nativeEvent?.isComposing || event.keyCode === 229;
const prefersReducedMotion = () => Boolean(
  window.matchMedia?.('(prefers-reduced-motion: reduce)')?.matches,
);

const SideChatMarkdown = memo(function SideChatMarkdown({ text, onMarkdownInteraction }) {
  const html = useMemo(() => ({ __html: renderMarkdown(text) }), [text]);
  return (
    <div
      className="ace-md"
      onClick={onMarkdownInteraction}
      onKeyDown={onMarkdownInteraction}
      dangerouslySetInnerHTML={html}
    />
  );
});

const SEND_STATUS_LABELS = {
  steered: '已插入主会话当前回合',
  started: '已发到主会话',
  queued: '已排到主会话下一轮',
  cancelled: '未发送',
};

// 「发到主会话」确认卡片:可编辑内容,确认后才送达;主会话运行中时按钮写明会插入当前回合。
function SideChatSendCard({ send, mainBusy, onEdit, onConfirm, onCancel }) {
  const editable = send.status === 'pending';
  return (
    <section className="ace-side-chat-send" data-send-status={send.status} aria-label="发到主会话">
      {editable ? (
        <textarea
          className="ace-scrollbar"
          value={send.text}
          rows={3}
          autoFocus
          aria-label="发到主会话的内容"
          onChange={(event) => onEdit?.(send.id, event.target.value)}
        />
      ) : (
        <div className="ace-side-chat-send-text">{send.text}</div>
      )}
      {editable ? (
        <div className="ace-side-chat-send-actions">
          <button type="button" onClick={() => onCancel?.(send.id)}>取消</button>
          <button
            type="button"
            className="is-primary"
            disabled={!send.text.trim()}
            onClick={() => onConfirm?.(send.id)}
          >
            {mainBusy ? '插入当前回合' : '发送'}
          </button>
        </div>
      ) : (
        <div
          className="ace-side-chat-send-status"
          role={send.status === 'failed' ? 'alert' : 'status'}
        >
          {send.status === 'sending' && <span className="ace-spinner" aria-hidden="true" />}
          <span>
            {send.status === 'sending'
              ? '正在发送…'
              : send.status === 'failed'
                ? (send.error || '发送失败')
                : SEND_STATUS_LABELS[send.status]}
          </span>
        </div>
      )}
    </section>
  );
}

function SideChatToolRow({ tool, send, sendProps }) {
  return (
    <li className="ace-side-chat-tool-item">
      <div className="ace-side-chat-tool" data-tool-status={tool.status}>
        {tool.status === 'running'
          ? <span className="ace-spinner" aria-hidden="true" />
          : <span className="ace-side-chat-tool-dot" aria-hidden="true" />}
        <span className="ace-side-chat-tool-verb">{sideChatToolVerb(tool.name)}</span>
        {tool.target && <span className="ace-side-chat-tool-target" title={tool.target}>{tool.target}</span>}
      </div>
      {send && <SideChatSendCard send={send} {...sendProps} />}
    </li>
  );
}

const SideChatTurn = memo(function SideChatTurn({
  turn,
  onMarkdownInteraction,
  mainBusy = false,
  onSendEdit,
  onSendConfirm,
  onSendCancel,
  onStartSend,
}) {
  const answer = String(turn.answer || '');
  const parts = useMemo(() => sideChatTurnParts(turn), [turn]);
  const generating = turn.status === 'loading' || turn.status === 'streaming';
  const sends = turn.sends || [];
  const toolIds = useMemo(() => new Set((turn.tools || []).map((tool) => tool.id)), [turn.tools]);
  const sendByCall = useMemo(
    () => new Map(sends.filter((send) => send.callId).map((send) => [send.callId, send])),
    [sends],
  );
  const looseSends = sends.filter((send) => !send.callId || !toolIds.has(send.callId));
  const sendProps = { mainBusy, onEdit: onSendEdit, onConfirm: onSendConfirm, onCancel: onSendCancel };
  const settled = turn.status === 'success' || turn.status === 'stopped';
  const handoffOpen = sends.some((send) => send.origin === 'user'
    && (send.status === 'pending' || send.status === 'sending'));
  const copyAnswer = async () => {
    try {
      await copyTextToClipboard(answer);
      toast({ kind: 'ok', text: '已复制回答' });
    } catch (error) {
      toast({ kind: 'err', text: '复制失败:' + (error?.message || '') });
    }
  };
  return (
    <article className="ace-side-chat-turn" data-side-chat-status={turn.status}>
      <div className="ace-side-chat-question">
        {turn.question}
        <AttachmentStrip attachments={turn.attachments || []} align="right" />
      </div>
      <div className="ace-side-chat-answer">
        {parts.map((part) => (part.kind === 'tools' ? (
          <ul key={part.key} className="ace-side-chat-tools" aria-label="只读工具调用">
            {part.tools.map((tool) => (
              <SideChatToolRow key={tool.id} tool={tool} send={sendByCall.get(tool.id)} sendProps={sendProps} />
            ))}
          </ul>
        ) : (
          <SideChatMarkdown key={part.key} text={part.text} onMarkdownInteraction={onMarkdownInteraction} />
        )))}
        {looseSends.map((send) => <SideChatSendCard key={send.id} send={send} {...sendProps} />)}
        {settled && answer.trim() && (
          <div className="ace-side-chat-answer-actions">
            <button type="button" aria-label="复制回答" title="复制回答" onClick={copyAnswer}>
              <VsIcon name="copy" size={14} />
            </button>
            {onStartSend && !handoffOpen && (
              <button
                type="button"
                aria-label="发到主会话"
                title="发到主会话"
                onClick={() => onStartSend(turn.id)}
              >
                <VsIcon name="ArrowUpRight" size={14} />
              </button>
            )}
          </div>
        )}
        {generating && (
          <div className="ace-side-chat-progress" role="status">
            <span className="ace-spinner" aria-hidden="true" />
            <span>{answer ? '正在回答…' : '思考中…'}</span>
          </div>
        )}
        {turn.status === 'stopped' && <div className="ace-side-chat-status">已停止</div>}
        {turn.status === 'error' && (
          <div className="ace-side-chat-error" role="alert">{turn.error || '旁路提问失败'}</div>
        )}
      </div>
    </article>
  );
});

export function SideChatWindow({
  open,
  anchor = null,
  turns = [],
  draft = '',
  busy = false,
  stopping = false,
  queued = [],
  queuePaused = false,
  composerProps = {},
  onResumeQueue,
  onDraftChange,
  onSubmit,
  onStop,
  onCancelQueued,
  onClear,
  onClose,
  onFileLink,
  fileLinkCwd = '',
  mainBusy = false,
  onSendEdit,
  onSendConfirm,
  onSendCancel,
  onStartSend,
  focusRequest = 0,
  // 顶栏「侧边聊天」按钮:打开时从它放大出来,最小化时缩回它。
  launcherRef = null,
  // 会话切换时窗口直接出现 / 消失,不播动画。
  motionKey = '',
}) {
  const titleId = useId();
  const dialogRef = useRef(null);
  const textareaRef = useRef(null);
  const closeRef = useRef(null);
  const transcriptRef = useRef(null);
  const pointerRef = useRef(null);
  const followingRef = useRef(true);
  const previousTurnCountRef = useRef(turns.length);
  const focusWithinRef = useRef(false);
  const [geometry, setGeometry] = useState(() => createSideChatGeometry(nativeSurfaceViewportRect()));
  const [interacting, setInteracting] = useState(false);
  // open 关闭后窗口还要留到缩回按钮的动画播完,所以挂载与否单独记。
  const [rendered, setRendered] = useState(open);
  const [motion, setMotion] = useState(null);
  const [presence, setPresence] = useState({ open, motionKey });
  const interruptedMotionRef = useRef(null);
  if (presence.open !== open || presence.motionKey !== motionKey) {
    const sameSession = presence.motionKey === motionKey;
    setPresence({ open, motionKey });
    if (open) {
      setRendered(true);
      setMotion(sameSession && !presence.open ? 'enter' : null);
    } else if (sameSession && rendered) {
      setMotion('exit');
    } else {
      setRendered(false);
      setMotion(null);
    }
  }
  useLayoutEffect(() => {
    if (!open) return undefined;
    setGeometry((current) => anchor
      ? anchorSideChatGeometry(current, anchor, nativeSurfaceViewportRect())
      : clampSideChatGeometry(current, nativeSurfaceViewportRect()));
    const updateViewport = () => setGeometry((current) => (
      clampSideChatGeometry(current, nativeSurfaceViewportRect())
    ));
    window.addEventListener('resize', updateViewport);
    window.visualViewport?.addEventListener('resize', updateViewport);
    window.visualViewport?.addEventListener('scroll', updateViewport);
    return () => {
      window.removeEventListener('resize', updateViewport);
      window.visualViewport?.removeEventListener('resize', updateViewport);
      window.visualViewport?.removeEventListener('scroll', updateViewport);
      pointerRef.current = null;
      setInteracting(false);
    };
  }, [open, anchor]);

  useLayoutEffect(() => {
    if (!rendered) return undefined;
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [rendered, geometry]);

  // 打开:等几何位置定下来(锚定到按钮下方是后一次同步渲染)再在首帧绘制前起动画;
  // 最小化:缩回按钮,播完才卸载。动画被反向操作打断时,从当前画面接着播。
  useLayoutEffect(() => {
    const element = dialogRef.current;
    if (!motion) {
      interruptedMotionRef.current = null;
      return undefined;
    }
    const kind = motion;
    if (!element || typeof element.animate !== 'function' || document.hidden) {
      interruptedMotionRef.current = null;
      if (kind === 'exit') setRendered(false);
      setMotion(null);
      return undefined;
    }
    const interrupted = interruptedMotionRef.current;
    interruptedMotionRef.current = null;
    let animation = null;
    let frame = 0;
    let disposed = false;
    const start = () => {
      frame = 0;
      element.style.removeProperty('opacity');
      const launcher = launcherRef?.current;
      const launcherRect = launcher?.isConnected
        ? usableSideChatLauncherRect(launcher.getBoundingClientRect(), nativeSurfaceViewportRect())
        : null;
      const plan = sideChatMotionPlan(kind, {
        box: element.getBoundingClientRect(),
        launcherRect,
        currentRect: interrupted?.rect,
        currentOpacity: interrupted?.opacity,
        reducedMotion: prefersReducedMotion(),
      });
      animation = element.animate(plan.keyframes, {
        duration: plan.duration,
        easing: plan.easing,
        fill: kind === 'exit' ? 'forwards' : 'none',
      });
      animation.onfinish = () => {
        if (disposed) return;
        if (kind === 'exit') {
          if (plan.usesLauncher) {
            launcher?.animate?.(SIDE_CHAT_LAUNCHER_PULSE.keyframes, {
              duration: SIDE_CHAT_LAUNCHER_PULSE.duration,
              easing: SIDE_CHAT_LAUNCHER_PULSE.easing,
            });
          }
          setRendered(false);
        } else {
          // 动画期间窗口外接矩形是缩放后的,播完再让原生浏览器 / 模拟器按完整窗口让位。
          notifyNativeSurfaceOverlayChange();
        }
        setMotion(null);
      };
    };
    if (kind === 'enter') {
      element.style.opacity = '0';
      frame = window.requestAnimationFrame(start);
    } else {
      start();
    }
    return () => {
      disposed = true;
      if (frame) window.cancelAnimationFrame(frame);
      if (animation?.playState === 'running') {
        interruptedMotionRef.current = {
          rect: element.getBoundingClientRect(),
          opacity: Number(window.getComputedStyle(element).opacity),
        };
      }
      animation?.cancel();
      element.style.removeProperty('opacity');
    };
  }, [motion]);

  useEffect(() => {
    if (!open) return undefined;
    const previousFocus = document.activeElement;
    const frame = window.requestAnimationFrame(() => {
      const input = textareaRef.current;
      (input && !input.disabled ? input : closeRef.current)?.focus({ preventScroll: true });
    });
    return () => {
      window.cancelAnimationFrame(frame);
      if (focusWithinRef.current && previousFocus?.isConnected) previousFocus.focus?.({ preventScroll: true });
    };
  }, [open]);

  // 主会话里「发到侧边对话」后,把光标放到引用之后,直接接着写问题。
  useEffect(() => {
    if (!open || !focusRequest) return undefined;
    const frame = window.requestAnimationFrame(() => {
      const input = textareaRef.current;
      if (!input || input.disabled) return;
      input.focus({ preventScroll: true });
    });
    return () => window.cancelAnimationFrame(frame);
  }, [open, focusRequest]);

  useLayoutEffect(() => {
    if (!open) return;
    const transcript = transcriptRef.current;
    if (turns.length > previousTurnCountRef.current) followingRef.current = true;
    previousTurnCountRef.current = turns.length;
    if (followingRef.current && transcript) transcript.scrollTop = transcript.scrollHeight;
  }, [open, turns, geometry.height, geometry.width]);

  const handleMarkdownInteraction = useCallback(async (event) => {
    if (event.type === 'keydown' && (isComposing(event) || (event.key !== 'Enter' && event.key !== ' '))) return;
    if (event.target?.closest?.('a[data-file-path]') && onFileLink) {
      event.preventDefault();
      event.stopPropagation();
      onFileLink(event);
      return;
    }
    // Keyboard activation of a copy button emits its own click; do not copy twice.
    if (event.type !== 'click') return;
    const text = codeTextFromCopyButtonTarget(event.target);
    if (text == null) return;
    event.preventDefault();
    event.stopPropagation();
    try {
      await copyTextToClipboard(text);
      toast({ kind: 'ok', text: '已复制代码' });
    } catch (error) {
      toast({ kind: 'err', text: '复制失败:' + (error?.message || '') });
    }
  }, [onFileLink]);

  const startPointer = (event, direction = '') => {
    if (event.button !== 0 || pointerRef.current) return;
    if (!direction && event.target.closest('button, a, input, textarea')) return;
    event.preventDefault();
    event.stopPropagation();
    event.currentTarget.setPointerCapture(event.pointerId);
    pointerRef.current = {
      pointerId: event.pointerId,
      x: event.clientX,
      y: event.clientY,
      geometry,
      direction,
    };
    setInteracting(true);
  };
  const movePointer = (event) => {
    const pointer = pointerRef.current;
    if (!pointer || pointer.pointerId !== event.pointerId) return;
    const dx = event.clientX - pointer.x;
    const dy = event.clientY - pointer.y;
    const viewport = nativeSurfaceViewportRect();
    setGeometry(pointer.direction
      ? resizeSideChatGeometry(pointer.geometry, pointer.direction, dx, dy, viewport)
      : moveSideChatGeometry(pointer.geometry, dx, dy, viewport));
  };
  const endPointer = (event) => {
    if (pointerRef.current?.pointerId !== event.pointerId) return;
    pointerRef.current = null;
    setInteracting(false);
    if (event.currentTarget.hasPointerCapture?.(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
  };
  const pointerHandlers = {
    onPointerMove: movePointer,
    onPointerUp: endPointer,
    onPointerCancel: endPointer,
    onLostPointerCapture: endPointer,
  };

  if (!rendered) return null;
  const windowElement = (
    <section
      ref={dialogRef}
      role="dialog"
      aria-modal="false"
      aria-labelledby={titleId}
      data-ace-native-overlay="overlap"
      data-side-chat-window="true"
      data-interacting={interacting ? 'true' : undefined}
      data-motion={motion || undefined}
      className="ace-side-chat-window"
      style={geometry}
      onFocusCapture={() => { focusWithinRef.current = true; }}
      onBlurCapture={(event) => {
        if (event.relatedTarget && !event.currentTarget.contains(event.relatedTarget)) focusWithinRef.current = false;
      }}
      onKeyDown={(event) => {
        if (!event.currentTarget.contains(event.target) || event.defaultPrevented) return;
        event.stopPropagation();
        if (isDismissShortcut(event) && !isComposing(event)) {
          event.preventDefault();
          onClose?.();
        }
      }}
      onKeyUp={(event) => event.stopPropagation()}
      onKeyPress={(event) => event.stopPropagation()}
      onClick={(event) => event.stopPropagation()}
    >
      <header className="ace-side-chat-header" onPointerDown={startPointer} {...pointerHandlers}>
        <h2 id={titleId}>侧边聊天</h2>
        <div className="ace-side-chat-header-actions">
          <button
            type="button"
            className="ace-side-chat-clear"
            aria-label="清空侧边聊天"
            title="清空侧边聊天"
            onClick={() => {
              onClear?.();
              followingRef.current = true;
              if (transcriptRef.current) transcriptRef.current.scrollTop = 0;
              textareaRef.current?.focus({ preventScroll: true });
            }}
          >
            <VsIcon name="delete" size={16} />
          </button>
          <button
            ref={closeRef}
            type="button"
            className="ace-side-chat-close"
            aria-label="最小化侧边聊天"
            title="最小化侧边聊天"
            onClick={onClose}
          >
            <VsIcon name="Minimize" size={16} />
          </button>
        </div>
      </header>
      <div
        ref={transcriptRef}
        className="ace-side-chat-transcript ace-scrollbar"
        // 文件链接右键可复制路径、在资源管理器中显示;不提供「添加到会话」,
        // 那会把文件塞进主会话的输入框。
        data-file-link-root="true"
        data-file-link-cwd={fileLinkCwd}
        aria-label="侧边聊天记录"
        onScroll={(event) => {
          const element = event.currentTarget;
          followingRef.current = element.scrollHeight - element.scrollTop - element.clientHeight < 48;
        }}
      >
        {turns.length === 0 ? (
          <div className="ace-side-chat-empty">围绕当前会话继续聊，不会打断主任务，也不会加入主会话。</div>
        ) : turns.map((turn) => (
          <SideChatTurn
            key={turn.id}
            turn={turn}
            onMarkdownInteraction={handleMarkdownInteraction}
            mainBusy={mainBusy}
            onSendEdit={onSendEdit}
            onSendConfirm={onSendConfirm}
            onSendCancel={onSendCancel}
            onStartSend={onStartSend}
          />
        ))}
      </div>
      {queued.length > 0 && (
        <div className="ace-side-chat-queue ace-scrollbar" role="list" aria-label="排队中的待发送消息">
          {queued.map((item) => (
            <div key={item.id} role="listitem" className="ace-queue-card flex items-center gap-2 px-3 py-2 text-[13px]">
              <span className="min-w-0 flex-1 truncate" title={item.text}>{item.text}</span>
              <span className="shrink-0 text-[11px] text-fg-mute">{queuePaused ? '已暂停' : '排队中'}</span>
              <button
                type="button"
                className="ace-queue-card-close shrink-0 w-6 h-6 rounded flex items-center justify-center"
                aria-label="取消排队"
                onClick={() => onCancelQueued?.(item.id)}
              >
                <VsIcon name="close" size={12} />
              </button>
            </div>
          ))}
        </div>
      )}
      <div className="ace-side-chat-composer">
        <InputBar
          {...composerProps}
          ref={textareaRef}
          aria-label="侧边聊天问题"
          placeholder="问一个不打断当前任务的问题…"
          value={draft}
          onChange={onDraftChange}
          onSubmit={onSubmit}
          busy={busy}
          stopping={stopping}
          onAbort={onStop}
          queuePaused={queuePaused}
          onResumeQueue={onResumeQueue}
          showAddControl={false}
        />
      </div>
      {RESIZE_DIRECTIONS.map((direction) => (
        <div
          key={direction}
          className={`ace-side-chat-resize ace-side-chat-resize-${direction}`}
          data-resize-direction={direction}
          aria-hidden="true"
          onPointerDown={(event) => startPointer(event, direction)}
          {...pointerHandlers}
        />
      ))}
    </section>
  );
  const content = <OverlayLayerContext.Provider value={2147483100}>{windowElement}</OverlayLayerContext.Provider>;
  return typeof document === 'undefined' ? content : createPortal(content, document.body);
}

export default SideChatWindow;
