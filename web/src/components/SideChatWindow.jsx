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
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const RESIZE_DIRECTIONS = ['n', 'e', 's', 'w', 'ne', 'se', 'sw', 'nw'];
const isComposing = (event) => event.isComposing || event.nativeEvent?.isComposing || event.keyCode === 229;

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

function SideChatToolRow({ tool }) {
  return (
    <li className="ace-side-chat-tool" data-tool-status={tool.status}>
      {tool.status === 'running'
        ? <span className="ace-spinner" aria-hidden="true" />
        : <span className="ace-side-chat-tool-dot" aria-hidden="true" />}
      <span className="ace-side-chat-tool-verb">{sideChatToolVerb(tool.name)}</span>
      {tool.target && <span className="ace-side-chat-tool-target" title={tool.target}>{tool.target}</span>}
    </li>
  );
}

const SideChatTurn = memo(function SideChatTurn({ turn, onMarkdownInteraction }) {
  const answer = String(turn.answer || '');
  const parts = useMemo(() => sideChatTurnParts(turn), [turn]);
  const generating = turn.status === 'loading' || turn.status === 'streaming';
  return (
    <article className="ace-side-chat-turn" data-side-chat-status={turn.status}>
      <div className="ace-side-chat-question">{turn.question}</div>
      <div className="ace-side-chat-answer">
        {parts.map((part) => (part.kind === 'tools' ? (
          <ul key={part.key} className="ace-side-chat-tools" aria-label="只读工具调用">
            {part.tools.map((tool) => <SideChatToolRow key={tool.id} tool={tool} />)}
          </ul>
        ) : (
          <SideChatMarkdown key={part.key} text={part.text} onMarkdownInteraction={onMarkdownInteraction} />
        )))}
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
  onDraftChange,
  onSubmit,
  onStop,
  onClear,
  onClose,
  onFileLink,
}) {
  const titleId = useId();
  const dialogRef = useRef(null);
  const textareaRef = useRef(null);
  const closeRef = useRef(null);
  const stopRef = useRef(null);
  const transcriptRef = useRef(null);
  const pointerRef = useRef(null);
  const followingRef = useRef(true);
  const previousTurnCountRef = useRef(turns.length);
  const focusWithinRef = useRef(false);
  const [geometry, setGeometry] = useState(() => createSideChatGeometry(nativeSurfaceViewportRect()));
  const [interacting, setInteracting] = useState(false);
  const locked = busy || stopping;
  const canSubmit = !locked && !!String(draft).trim();

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
    if (!open) return undefined;
    notifyNativeSurfaceOverlayChange();
    return () => notifyNativeSurfaceOverlayChange();
  }, [open, geometry]);

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

  useLayoutEffect(() => {
    if (!open || !focusWithinRef.current) return;
    // Disabling a focused textarea otherwise leaves focus on body, where Escape
    // can reach the main task's stop shortcut while a side answer is running.
    const target = locked
      ? (stopping ? closeRef.current : stopRef.current)
      : textareaRef.current;
    target?.focus({ preventScroll: true });
  }, [open, locked, stopping]);

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

  if (!open) return null;
  const windowElement = (
    <section
      ref={dialogRef}
      role="dialog"
      aria-modal="false"
      aria-labelledby={titleId}
      data-ace-native-overlay="overlap"
      data-side-chat-window="true"
      data-interacting={interacting ? 'true' : undefined}
      className="ace-side-chat-window"
      style={geometry}
      onFocusCapture={() => { focusWithinRef.current = true; }}
      onBlurCapture={(event) => {
        if (event.relatedTarget && !event.currentTarget.contains(event.relatedTarget)) focusWithinRef.current = false;
      }}
      onKeyDown={(event) => {
        event.stopPropagation();
        if (event.key === 'Escape' && !isComposing(event)) {
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
        aria-label="侧边聊天记录"
        onScroll={(event) => {
          const element = event.currentTarget;
          followingRef.current = element.scrollHeight - element.scrollTop - element.clientHeight < 48;
        }}
      >
        {turns.length === 0 ? (
          <div className="ace-side-chat-empty">围绕当前会话继续聊，不会打断主任务，也不会加入主会话。</div>
        ) : turns.map((turn) => (
          <SideChatTurn key={turn.id} turn={turn} onMarkdownInteraction={handleMarkdownInteraction} />
        ))}
      </div>
      <form className="ace-side-chat-composer" onSubmit={(event) => {
        event.preventDefault();
        if (canSubmit) onSubmit?.();
      }}>
        <textarea
          ref={textareaRef}
          className="ace-scrollbar"
          value={draft}
          rows={1}
          disabled={locked}
          aria-label="侧边聊天问题"
          placeholder="问一个不打断当前任务的问题…"
          onChange={(event) => onDraftChange?.(event.target.value)}
          onKeyDown={(event) => {
            if (event.key === 'Enter' && !event.shiftKey && !isComposing(event)) {
              event.preventDefault();
              if (canSubmit && !event.repeat) onSubmit?.();
            }
          }}
        />
        {locked ? (
          <button
            ref={stopRef}
            type="button"
            className="ace-side-chat-action"
            aria-label={stopping ? '正在停止…' : '停止生成'}
            title={stopping ? '正在停止…' : '停止生成'}
            disabled={stopping}
            onClick={onStop}
          >
            {stopping ? <span className="ace-spinner" aria-hidden="true" /> : <VsIcon name="stop" size={16} />}
          </button>
        ) : (
          <button type="submit" className="ace-side-chat-action" aria-label="发送" title="发送" disabled={!canSubmit}>
            <VsIcon name="send" size={16} />
          </button>
        )}
      </form>
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
  return typeof document === 'undefined' ? windowElement : createPortal(windowElement, document.body);
}

export default SideChatWindow;
