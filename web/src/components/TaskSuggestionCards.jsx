import { isDismissShortcut } from '../lib/shortcutBindings.js';
import { useEffect, useRef, useState } from 'react';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { VsIcon } from './Icon.jsx';
import { createTaskSuggestionsController, suggestionTargetRef, SUGGESTION_DISMISS_DELAY_MS } from '../lib/taskSuggestions.js';
import { notifySessionListChanged } from '../lib/sessionListEvents.js';
import '../styles/task-suggestions.css';

const ACTION_CLASS = 'min-h-8 px-3 py-1.5 rounded-md text-[12px] font-medium hover:bg-surface-hi focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent disabled:opacity-50 disabled:cursor-default';

function TaskSuggestionFooter({ deadline, children }) {
  const active = Number.isFinite(deadline);
  const [initialRemaining] = useState(() => active ? Math.max(0, deadline - Date.now()) : 0);
  const [seconds, setSeconds] = useState(() => Math.ceil(initialRemaining / 1000));

  useEffect(() => {
    if (!active) return undefined;
    const update = () => setSeconds(Math.max(0, Math.ceil((deadline - Date.now()) / 1000)));
    update();
    const timer = setInterval(update, 250);
    return () => clearInterval(timer);
  }, [active, deadline]);

  return (
    <div
      className="ace-suggestion-countdown"
      data-urgent={active && seconds <= 5 ? 'true' : undefined}
      style={{
        '--ace-countdown-start': initialRemaining / SUGGESTION_DISMISS_DELAY_MS,
        '--ace-countdown-step': seconds * 1000 / SUGGESTION_DISMISS_DELAY_MS,
        '--ace-countdown-duration': `${initialRemaining}ms`,
      }}
    >
      <div className="flex flex-wrap items-end justify-end gap-x-3 gap-y-2">
        {active && (
          <span className="ace-suggestion-countdown-label" role="timer" aria-live="off">
            {`${seconds}秒后关闭`}
          </span>
        )}
        <div className="ml-auto flex max-w-full flex-wrap items-center justify-end gap-2">
          {children}
        </div>
      </div>
      {active && (
        <div className="ace-suggestion-countdown-track" aria-hidden="true">
          <div className="ace-suggestion-countdown-fill" />
          <div className="ace-suggestion-countdown-cursor" />
        </div>
      )}
    </div>
  );
}

function TaskSuggestionCard({ suggestion, state, controller, sourceRef, onOpenSession }) {
  const [location, setLocation] = useState(suggestion.location || 'worktree');
  const [menuOpen, setMenuOpen] = useState(false);
  const menuAnchor = useRef(null);
  const handoff = suggestion.kind === 'context_handoff';
  const title = handoff ? '在新会话中继续' : suggestion.title || '后台任务建议';
  const description = handoff
    ? '当前会话已多次压缩，建议在新会话中接续当前工作。'
    : suggestion.description;
  const pendingAction = state.pending[suggestion.id];
  const starting = suggestion.status === 'starting';
  const queued = suggestion.status === 'queued';
  const started = suggestion.status === 'started';
  const actionDisabled = !!pendingAction || starting;
  const error = state.errors[suggestion.id] || suggestion.error;
  const retry = suggestion.status === 'failed'
    || (!!state.errors[suggestion.id] && state.errorActions[suggestion.id] === 'accept');
  const selectedLocation = handoff ? 'current_branch'
    : suggestion.location || (state.worktreeAvailable ? location : 'current_branch');
  const target = suggestionTargetRef(suggestion, sourceRef);
  const closeLabel = queued ? '取消排队' : started ? '关闭建议' : '忽略建议';
  const startLabel = handoff ? '新建会话并继续'
    : selectedLocation === 'worktree' ? '在 worktree 中开始' : '在当前分支开始';
  const dismiss = () => { if (!actionDisabled) void controller.dismiss(suggestion.id); };

  return (
    <section
      aria-label={handoff ? '会话续接建议' : '后台任务建议'}
      data-task-suggestion={suggestion.kind}
      data-suggestion-status={suggestion.status}
      className="pointer-events-auto rounded-xl bg-surface border border-border p-4 text-fg"
      style={{ boxShadow: 'var(--ace-shadow-lg), var(--ace-shadow)' }}
      onKeyDown={(event) => {
        if (isDismissShortcut(event) && !menuOpen && !actionDisabled) {
          event.preventDefault();
          event.stopPropagation();
          dismiss();
        }
      }}
    >
      <div className="flex items-start justify-between gap-3">
        <h2 className="min-w-0 text-[14px] font-semibold leading-5 break-words [overflow-wrap:anywhere]">
          {title}
        </h2>
        <button
          type="button"
          aria-label={closeLabel}
          title={closeLabel}
          disabled={actionDisabled}
          onClick={dismiss}
          className="-mr-1 -mt-1 flex h-7 w-7 shrink-0 items-center justify-center rounded-md text-fg-mute hover:bg-surface-hi hover:text-fg focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent disabled:opacity-50"
        >
          <VsIcon name="close" size={14} />
        </button>
      </div>
      {description && (
        <p className="mt-2 text-[13px] leading-5 text-fg-2 whitespace-pre-wrap [overflow-wrap:anywhere]">
          {description}
        </p>
      )}
      {!started && !queued && !starting && !handoff && (
        <p className="mt-3 text-[11px] leading-4 text-fg-mute">
          {selectedLocation === 'worktree'
            ? '创建独立 worktree，不包含未提交的修改。'
            : '与当前会话共享文件；有任务运行时会排队。'}
        </p>
      )}
      {(queued || starting || started || pendingAction === 'accept') && (
        <p className="mt-3 text-[12px] leading-5 text-fg-mute" role="status">
          {started ? (handoff ? '已转至新会话继续。' : '任务已启动。')
            : queued ? '已排队，等待当前目录中的任务结束。'
              : '正在创建会话...'}
        </p>
      )}
      {error && (
        <p className="mt-3 text-[12px] leading-5 text-danger [overflow-wrap:anywhere]" role="alert">
          <span>{state.errorActions[suggestion.id] === 'dismiss' ? '无法关闭建议：' : '操作未完成：'}</span>{error}
        </p>
      )}
      <TaskSuggestionFooter
        key={state.dismissDeadlines[suggestion.id] ?? 'stopped'}
        deadline={state.dismissDeadlines[suggestion.id]}
      >
        {started && target && (
          <button type="button" className={`${ACTION_CLASS} text-accent`} onClick={() => onOpenSession?.(target)}>
            打开会话
            <VsIcon name="arrowRight" size={12} className="ml-1.5 inline-block" />
          </button>
        )}
        {queued && (
          <button type="button" disabled={actionDisabled} className={`${ACTION_CLASS} text-fg-mute`} onClick={dismiss}>
            {pendingAction === 'dismiss' ? '正在取消...' : '取消排队'}
          </button>
        )}
        {!started && !queued && !starting && (
          <div className="flex max-w-full items-stretch rounded-md border border-border bg-surface">
            <button
              type="button"
              disabled={actionDisabled}
              onClick={() => void controller.accept(suggestion.id, selectedLocation)}
              className={`${ACTION_CLASS} min-w-0`}
            >
              {pendingAction === 'accept' ? '正在创建会话...' : retry ? '重试' : startLabel}
            </button>
            {!handoff && !suggestion.location && (
              <>
                <button
                  ref={menuAnchor}
                  type="button"
                  aria-label="选择任务位置"
                  aria-haspopup="menu"
                  aria-expanded={menuOpen}
                  disabled={actionDisabled}
                  onClick={() => setMenuOpen((value) => !value)}
                  className="w-8 shrink-0 rounded-r-md border-l border-border flex items-center justify-center hover:bg-surface-hi focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent disabled:opacity-50"
                >
                  <VsIcon name="expandDown" size={12} />
                </button>
                {menuOpen && (
                  <AnchoredMenu
                    anchorRef={menuAnchor}
                    onClose={() => setMenuOpen(false)}
                    width={260}
                    role="menu"
                    aria-label="选择任务位置"
                    className="z-50 rounded-lg border border-border bg-surface p-1 ace-shadow-lg"
                  >
                    {[
                      { value: 'worktree', label: '在 worktree 中开始', disabled: !state.worktreeAvailable },
                      { value: 'current_branch', label: '在当前分支开始', disabled: false },
                    ].map((option) => (
                      <button
                        key={option.value}
                        type="button"
                        role="menuitemradio"
                        aria-checked={selectedLocation === option.value}
                        disabled={option.disabled}
                        title={option.disabled ? '当前目录无法创建 worktree' : undefined}
                        className="flex w-full items-center gap-2 rounded-md px-3 py-2 text-left text-[12px] text-fg hover:bg-surface-hi focus-visible:outline-none focus-visible:bg-surface-hi disabled:opacity-50"
                        onClick={() => { setLocation(option.value); setMenuOpen(false); }}
                      >
                        <VsIcon name={selectedLocation === option.value ? 'check' : 'fork'} size={13} />
                        <span>{option.label}</span>
                      </button>
                    ))}
                  </AnchoredMenu>
                )}
              </>
            )}
          </div>
        )}
      </TaskSuggestionFooter>
    </section>
  );
}

export function TaskSuggestionCards({ api, sessionId, sourceRef, busy, onOpenSession }) {
  const [state, setState] = useState(null);
  const controllerRef = useRef(null);
  const runtimeRef = useRef({ sourceRef, onOpenSession });
  runtimeRef.current = { sourceRef, onOpenSession };

  useEffect(() => {
    const controller = createTaskSuggestionsController({
      api, sessionId, busy,
      onChange: setState,
      onStarted: (suggestion, { continueInTarget }) => {
        const target = suggestionTargetRef(suggestion, runtimeRef.current.sourceRef);
        if (!target) return;
        notifySessionListChanged({ reason: 'task-suggestion', ...target });
        if (continueInTarget) runtimeRef.current.onOpenSession?.(target);
      },
    });
    controllerRef.current = controller;
    setState(controller.getSnapshot());
    void controller.refresh();
    return () => { controller.dispose(); controllerRef.current = null; };
  }, [api, sessionId]);

  useEffect(() => { controllerRef.current?.setBusy(busy); }, [busy]);

  if (!state?.suggestions.length || !controllerRef.current) return null;
  return (
    <aside
      aria-label="任务建议"
      // Reserve shadow space inside the scroll area while keeping card bounds unchanged.
      className="pointer-events-none absolute right-0 top-9 z-30 flex max-h-[calc(100%-2.5rem)] w-full max-w-[27.5rem] flex-col gap-3 overflow-y-auto overscroll-contain p-3 ace-scrollbar"
    >
      {state.suggestions.map((suggestion) => (
        <TaskSuggestionCard
          key={suggestion.id}
          suggestion={suggestion}
          state={state}
          controller={controllerRef.current}
          sourceRef={sourceRef}
          onOpenSession={onOpenSession}
        />
      ))}
      {state.refreshError && (
        <div className="pointer-events-auto rounded-md border border-border bg-surface px-3 py-2 text-[12px] text-fg-2" role="status">
          <span>建议状态更新失败。</span>
          <button
            type="button"
            className={`${ACTION_CLASS} text-accent`}
            onClick={() => void controllerRef.current?.retryRefresh()}
          >
            重新加载
          </button>
        </div>
      )}
    </aside>
  );
}
