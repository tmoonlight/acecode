// 「后台任务」面板(spawn_subagent 子会话)。
//
// 归属主会话窗口:作为 ChatView transcript 区(flex 行)的可调 split pane,
// 打开时**挤压**聊天消息区(而非浮层遮挡),关闭时返回 null 不占位;只影响
// 聊天会话区,不动 Sidebar/右侧文件预览面板、也不压输入框(输入框在 transcript
// 区之外仍占满宽)。splitter 与持久化宽度由 ChatView/App 持有,本组件只按
// 已约束的 width 渲染内容。
//
// 两个视图:
//   - 列表:运行中 / 已完成 分组卡片。运行中卡片右上有中止(stop);
//     已完成组标题行有「清除」(purge 全部已结束任务,永久删除)。
//   - transcript:点卡片「查看会话」原地切换,复用主会话的完整 transcript
//     投影与渲染链路,仅通过能力开关保持只读。AskUserQuestion 工具行不显示——
//     子代理的提问/权限确认冒泡到主会话 UI 回答,这里只看执行过程。

import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
} from 'react';
import { clsx } from '../lib/format.js';
import { DEFAULT_SUBAGENT_PANEL_WIDTH } from '../lib/singleLayout.js';
import { useSessionTranscript } from '../lib/sessionTranscript.js';
import {
  CHAT_TAIL_FOLLOW_STATE,
  chatScrollMetrics,
  nextChatTailFollowState,
  observeChatTailContent,
  shouldAutoFollowChatTail,
} from '../lib/chatScrollFollow.js';
import { READ_ONLY_TRANSCRIPT_CAPABILITIES } from '../lib/transcriptItemPresentation.js';
import { projectSubagentTranscriptItems } from '../lib/subagentTranscript.js';
import {
  SUBAGENT_TASK_STATUS,
  formatElapsed,
  subagentTaskGroups,
  taskDisplayTitle,
  taskElapsedSeconds,
  taskStatsParts,
  taskStatusLabel,
} from '../lib/subagentTasks.js';
import { VsIcon } from './Icon.jsx';
import { TranscriptItems } from './TranscriptItems.jsx';

function TaskCard({ task, nowMs, onAbort, onOpenTranscript }) {
  const running = task.status === SUBAGENT_TASK_STATUS.RUNNING;
  const stats = taskStatsParts(task);
  const elapsed = formatElapsed(taskElapsedSeconds(task, nowMs));
  return (
    <div className="rounded-lg border border-border bg-surface px-3 py-2.5 flex flex-col gap-1 ace-shadow">
      <div className="flex items-start gap-2">
        <span className="flex-1 min-w-0 text-[12.5px] font-semibold text-fg break-words">
          {taskDisplayTitle(task)}
        </span>
        {running && (
          <button
            type="button"
            onClick={() => onAbort?.(task)}
            className="w-6 h-6 shrink-0 rounded-md border border-border text-fg-mute flex items-center justify-center transition hover:text-danger hover:border-danger/40 hover:bg-danger-bg"
            title="中止任务"
            aria-label="中止任务"
          >
            <VsIcon name="stop" size={12} />
          </button>
        )}
      </div>
      <div className="flex items-center gap-1.5 text-[11px] text-fg-2">
        <span>Agent</span>
        {running ? (
          <>
            <span className="w-1.5 h-1.5 rounded-full bg-ok shadow-[0_0_5px_var(--ace-ok)]" />
            <span className="tabular-nums">{elapsed}</span>
          </>
        ) : (
          <span className="text-fg-mute">
            {taskStatusLabel(task)} {elapsed}
          </span>
        )}
      </div>
      {stats.length > 0 && (
        <div className="text-[11px] text-fg-mute truncate" title={stats.join(' · ')}>
          {stats.join(' · ')}
        </div>
      )}
      <button
        type="button"
        onClick={() => onOpenTranscript?.(task)}
        className="self-start text-[11.5px] text-accent hover:underline"
      >
        查看会话
      </button>
    </div>
  );
}

function SubagentTranscriptView({ task }) {
  const sessionRef = useMemo(() => ({
    sessionId: task.id,
    busy: task.status === SUBAGENT_TASK_STATUS.RUNNING,
    title: taskDisplayTitle(task),
  }), [task.id, task.status, task.title, task.summary]);
  const transcript = useSessionTranscript(sessionRef, { live: 'auto' });
  const running = transcript.busy || task.status === SUBAGENT_TASK_STATUS.RUNNING;
  const items = useMemo(
    () => projectSubagentTranscriptItems(transcript.items, {
      deferTrailingToolSummary: running,
      ensureLiveActivity: running,
      liveTurnId: `subagent:${task.id}`,
    }),
    [running, task.id, transcript.items],
  );

  const [expandedActivityKeys, setExpandedActivityKeys] = useState(() => new Set());
  const [collapsedMediaKeys, setCollapsedMediaKeys] = useState(() => new Set());
  const scrollRef = useRef(null);
  const contentRef = useRef(null);
  const tailFollowStateRef = useRef(CHAT_TAIL_FOLLOW_STATE.FOLLOWING);
  const scrollActivityRef = useRef({ prev: null, pointerActive: false });
  const tailFollowRafRef = useRef({ first: 0, second: 0 });

  const setTailFollowFromAction = useCallback((action) => {
    tailFollowStateRef.current = nextChatTailFollowState(tailFollowStateRef.current, action);
  }, []);

  const cancelTailFollowScroll = useCallback(() => {
    const pending = tailFollowRafRef.current || {};
    if (pending.first) cancelAnimationFrame(pending.first);
    if (pending.second) cancelAnimationFrame(pending.second);
    tailFollowRafRef.current = { first: 0, second: 0 };
  }, []);

  const scheduleTailFollowScroll = useCallback(() => {
    const scrollToBottom = () => {
      if (!shouldAutoFollowChatTail(tailFollowStateRef.current)) return false;
      const el = scrollRef.current;
      if (!el) return false;
      el.scrollTop = el.scrollHeight;
      return true;
    };

    cancelTailFollowScroll();
    if (!scrollToBottom()) return;
    tailFollowRafRef.current.first = requestAnimationFrame(() => {
      tailFollowRafRef.current.first = 0;
      if (!scrollToBottom()) return;
      tailFollowRafRef.current.second = requestAnimationFrame(() => {
        tailFollowRafRef.current.second = 0;
        scrollToBottom();
      });
    });
  }, [cancelTailFollowScroll]);

  const pauseTailFollowForReview = useCallback(() => {
    cancelTailFollowScroll();
    if (!running) return;
    setTailFollowFromAction({ type: 'review_pause' });
  }, [cancelTailFollowScroll, running, setTailFollowFromAction]);

  const handleScroll = useCallback(() => {
    const el = scrollRef.current;
    if (!el) return;
    const metrics = chatScrollMetrics(el);
    setTailFollowFromAction({
      type: 'scroll',
      metrics,
      prevMetrics: scrollActivityRef.current.prev,
      userGesture: scrollActivityRef.current.pointerActive,
    });
    scrollActivityRef.current.prev = metrics;
  }, [setTailFollowFromAction]);

  const toggleActivitySummary = useCallback((key) => {
    pauseTailFollowForReview();
    setExpandedActivityKeys((prev) => {
      const next = new Set(prev);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  }, [pauseTailFollowForReview]);

  const toggleMediaGroup = useCallback((key) => {
    pauseTailFollowForReview();
    setCollapsedMediaKeys((prev) => {
      const next = new Set(prev);
      if (next.has(key)) next.delete(key);
      else next.add(key);
      return next;
    });
  }, [pauseTailFollowForReview]);

  useLayoutEffect(() => {
    setTailFollowFromAction({ type: 'session_reset' });
    scrollActivityRef.current = { prev: null, pointerActive: false };
    setExpandedActivityKeys(new Set());
    setCollapsedMediaKeys(new Set());
  }, [setTailFollowFromAction, task.id]);

  useLayoutEffect(() => {
    scheduleTailFollowScroll();
    return cancelTailFollowScroll;
  }, [cancelTailFollowScroll, items, scheduleTailFollowScroll]);

  useEffect(() => observeChatTailContent(
    contentRef.current,
    scheduleTailFollowScroll,
  ), [scheduleTailFollowScroll, task.id]);

  useEffect(() => {
    const clearPointerActive = () => {
      scrollActivityRef.current.pointerActive = false;
    };
    window.addEventListener('pointerup', clearPointerActive);
    window.addEventListener('pointercancel', clearPointerActive);
    return () => {
      window.removeEventListener('pointerup', clearPointerActive);
      window.removeEventListener('pointercancel', clearPointerActive);
    };
  }, []);

  return (
    <div
      ref={scrollRef}
      onScroll={handleScroll}
      onWheel={(event) => {
        if (event.deltaY < 0) pauseTailFollowForReview();
      }}
      onPointerDown={() => {
        scrollActivityRef.current.pointerActive = true;
      }}
      className="ace-subagent-transcript flex-1 min-h-0 overflow-y-auto px-2.5 py-2 flex flex-col gap-2"
    >
      <div ref={contentRef} className="flex flex-col gap-2">
        {transcript.loadState === 'loading' && (
          <div className="text-[12px] text-fg-mute px-1 py-2 flex items-center gap-2">
            <span className="ace-spinner" /> 加载会话记录…
          </div>
        )}
        {transcript.loadState === 'error' && (
          <div className="text-[12px] text-danger px-1 py-2">加载失败:{transcript.error || ''}</div>
        )}
        <TranscriptItems
          items={items}
          capabilities={READ_ONLY_TRANSCRIPT_CAPABILITIES}
          expandedActivityKeys={expandedActivityKeys}
          collapsedMediaKeys={collapsedMediaKeys}
          onToggleActivity={toggleActivitySummary}
          onToggleMedia={toggleMediaGroup}
          sessionRunning={running}
          onReviewToggle={pauseTailFollowForReview}
        />
        {transcript.loadState === 'loaded' && items.length === 0 && (
          <div className="text-[12px] text-fg-mute px-1 py-2">暂无会话内容</div>
        )}
      </div>
    </div>
  );
}

export function SubagentPanel({ open, width = DEFAULT_SUBAGENT_PANEL_WIDTH, focus, onClose, tasks, onAbort, onClearSettled }) {
  const [transcriptTaskId, setTranscriptTaskId] = useState('');
  const [clearing, setClearing] = useState(false);

  // 面板关闭后回到列表视图,重开不残留上一次的 transcript。
  useEffect(() => {
    if (!open) setTranscriptTaskId('');
  }, [open]);

  // 外部(聊天流「调用了 N 个智能体」分组点某个智能体)请求定位到某子会话。
  // focus.n 单调递增,同一 id 的重复点击也会重新触发。
  useEffect(() => {
    if (focus?.id) setTranscriptTaskId(focus.id);
  }, [focus?.n, focus?.id]);

  const groups = useMemo(() => subagentTaskGroups(tasks), [tasks]);
  // 目标任务不在列表(如已清除但聊天流仍留有分组项)时,合成一个最小任务对象,
  // transcript 仍能按 session_id 拉取展示。
  const transcriptTask = transcriptTaskId
    ? (tasks.find((t) => t.id === transcriptTaskId)
       || { id: transcriptTaskId, status: SUBAGENT_TASK_STATUS.COMPLETED, title: '', summary: '' })
    : null;

  // 运行中卡片的耗时每秒 tick(仅面板打开且列表视图有运行中任务时)。
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    if (!open || transcriptTask || groups.running.length === 0) return undefined;
    const timer = setInterval(() => setNowMs(Date.now()), 1000);
    return () => clearInterval(timer);
  }, [open, transcriptTask, groups.running.length]);

  if (!open) return null;

  const clearSettled = async () => {
    if (clearing) return;
    setClearing(true);
    try {
      await onClearSettled?.();
    } finally {
      setClearing(false);
    }
  };

  return (
    <div
      className="shrink-0 min-w-0 h-full flex flex-col bg-surface"
      style={{ width }}
      data-subagent-panel="true"
    >
      <div className="h-10 px-3 flex items-center gap-2 shrink-0">
        {transcriptTask ? (
          <>
            <button
              type="button"
              onClick={() => setTranscriptTaskId('')}
              className="w-7 h-7 rounded-md text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg"
              title="返回任务列表"
              aria-label="返回任务列表"
            >
              <VsIcon name="arrowLeft" size={14} />
            </button>
            <span className="flex-1 min-w-0 text-[13px] font-semibold text-fg truncate">
              {taskDisplayTitle(transcriptTask)}
            </span>
          </>
        ) : (
          <>
            <VsIcon name="embedding" size={15} className="text-fg-2" />
            <span className="flex-1 text-[13px] font-semibold text-fg">后台任务</span>
          </>
        )}
        <button
          type="button"
          onClick={onClose}
          className="w-7 h-7 rounded-md text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg"
          title="关闭"
          aria-label="关闭后台任务面板"
        >
          <VsIcon name="close" size={14} />
        </button>
      </div>

      {transcriptTask ? (
        <SubagentTranscriptView key={transcriptTask.id} task={transcriptTask} />
      ) : (
        <div className="flex-1 min-h-0 overflow-y-auto px-3 py-3 flex flex-col gap-3">
          {tasks.length === 0 && (
            <div className="text-[12px] text-fg-mute py-4 text-center">
              当前会话还没有后台任务。
              <br />让代理调用 spawn_subagent 即可在这里看到。
            </div>
          )}
          {groups.running.length > 0 && (
            <div className="flex flex-col gap-2">
              <div className="text-[11.5px] font-medium text-fg-2">运行中</div>
              {groups.running.map((task) => (
                <TaskCard
                  key={task.id}
                  task={task}
                  nowMs={nowMs}
                  onAbort={onAbort}
                  onOpenTranscript={(t) => setTranscriptTaskId(t.id)}
                />
              ))}
            </div>
          )}
          {groups.settled.length > 0 && (
            <div className="flex flex-col gap-2">
              <div className="flex items-center justify-between">
                <span className="text-[11.5px] font-medium text-fg-2">
                  已完成 {groups.settled.length}
                </span>
                <button
                  type="button"
                  onClick={clearSettled}
                  disabled={clearing}
                  className={clsx(
                    'text-[11.5px] text-fg-mute transition hover:text-danger',
                    clearing && 'opacity-50 cursor-default',
                  )}
                  title="永久删除全部已结束任务(不影响主会话)"
                >
                  {clearing ? '清除中…' : '清除'}
                </button>
              </div>
              {groups.settled.map((task) => (
                <TaskCard
                  key={task.id}
                  task={task}
                  nowMs={nowMs}
                  onAbort={onAbort}
                  onOpenTranscript={(t) => setTranscriptTaskId(t.id)}
                />
              ))}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
