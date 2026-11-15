// 「后台任务」页签内容(后台命令 + spawn_subagent 子会话)。
//
// 挂在详情栏(PreviewDetailsPanel)的「后台任务」页签里,每个会话一个页签。
// 正在看哪个任务(taskId)记在页签状态里,由 ChatView 经 onSelectTask 更新,
// 切到别的页签再切回来仍停在原处。
//
// 两个视图:
//   - 列表:运行中 / 已完成 两组。所有任务共用无边框的行(图标、标题、时间靠右;
//     运行中多一行状态)。子代理显示 token 数,后台命令用固定控制台图标且不显示 token。
//     点整行看子会话或命令输出。
//     子会话没有单独归档 / 删除入口:它们始终跟随主会话留在这里,
//     只在主会话归档后被永久删除时一起删除(用户决策,事后分析要完整上下文)。
//   - transcript:点子代理那一行原地切换,复用主会话的完整 transcript
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
import { useSessionTranscript } from '../lib/sessionTranscript.js';
import { createApi } from '../lib/api.js';
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
  subagentAvatarStatus,
  subagentTaskGroups,
  taskDisplayTitle,
  taskRunningStatusLabel,
  taskTimeLabel,
  taskTokensLabel,
} from '../lib/subagentTasks.js';
import { VsIcon } from './Icon.jsx';
import { commandTaskStatusLabel, isCommandTask } from '../lib/backgroundTasks.js';
import { TranscriptItems } from './TranscriptItems.jsx';
import { AttachmentTextLoaderContext } from './AttachmentTextLoaderContext.jsx';
import { SubagentAvatar } from './PixelAvatar.jsx';

// 后台命令与子代理共用任务行,仅图标和 token 显示不同。
// 点整行看会话或输出;中止按钮只在悬停 / 聚焦时出现(触屏没有悬停,一直显示)。
// 网状蜂群的 agent 路径不占位置,放在悬停提示里(agent 之间按路径互相称呼)。
function TaskRow({ task, nowMs, onStop, onOpen }) {
  const command = isCommandTask(task);
  const running = task.status === SUBAGENT_TASK_STATUS.RUNNING;
  const stopPending = command ? task.stopPending : task.abortPending;
  const tokens = command ? '' : taskTokensLabel(task);
  const status = taskRunningStatusLabel(command ? { ...task, abortPending: stopPending } : task);
  const title = command ? task.command : taskDisplayTitle(task);
  return (
    <div className="group/task -mx-2 flex items-start gap-1 min-w-0"
      data-background-command={command ? task.id : undefined}
      data-background-subagent={command ? undefined : task.id}>
      <button
        type="button"
        onClick={() => onOpen?.(task)}
        title={(command ? task.command : task.agentPath) || undefined}
        className="flex-1 min-w-0 flex items-start gap-2.5 px-2 py-1.5 rounded-md text-left transition hover:bg-surface-hi"
      >
        {command ? (
          <VsIcon name="terminal" size={16} className="mt-0.5 shrink-0 text-fg-2" />
        ) : (
          <SubagentAvatar sessionId={task.id} expert={task.expert} status={task.status === SUBAGENT_TASK_STATUS.COMPLETED ? '' : subagentAvatarStatus(task)} className="mt-0.5" />
        )}
        <span className="flex-1 min-w-0 flex flex-col">
          <span className="flex items-baseline gap-2 min-w-0 leading-5">
            <span className="min-w-0 truncate text-[13px] text-fg">{title}</span>
            {tokens && <span className="shrink-0 text-[12px] text-fg-mute tabular-nums">{tokens}</span>}
          </span>
          {status && <span className="text-[12px] leading-5 text-fg-mute" data-task-status="true">{status}</span>}
        </span>
        <span className="shrink-0 text-[12px] leading-5 text-fg-mute tabular-nums" data-task-time="true">
          {taskTimeLabel(task, nowMs)}
        </span>
      </button>
      {running && (
        <button
          type="button"
          onClick={() => onStop?.(task)}
          disabled={stopPending}
          className="mt-1 w-6 h-6 shrink-0 rounded-md text-fg-mute flex items-center justify-center transition opacity-0 group-hover/task:opacity-100 focus-visible:opacity-100 [@media(hover:none)]:opacity-100 hover:text-danger hover:bg-danger-bg"
          title="中止任务"
          aria-label="中止任务"
        >
          <VsIcon name="stop" size={12} />
        </button>
      )}
    </div>
  );
}

// 后台命令的输出末尾:运行中每 2 秒刷新一次。
function CommandOutputView({ task, readOutput }) {
  const [output, setOutput] = useState('');
  const [outputFile, setOutputFile] = useState(task?.outputFile || '');
  const running = task?.status === SUBAGENT_TASK_STATUS.RUNNING;
  const scrollRef = useRef(null);
  useEffect(() => {
    let cancelled = false;
    let timer = null;
    const load = async () => {
      const response = await readOutput?.(task.id);
      if (cancelled) return;
      if (response) {
        setOutput(String(response.output || ''));
        if (response.task?.output_file) setOutputFile(response.task.output_file);
      }
      if (running) timer = setTimeout(load, 2000);
    };
    load();
    return () => {
      cancelled = true;
      if (timer) clearTimeout(timer);
    };
  }, [task?.id, running, readOutput]);
  useLayoutEffect(() => {
    const node = scrollRef.current;
    if (node) node.scrollTop = node.scrollHeight;
  }, [output]);
  return (
    <div className="flex-1 min-h-0 flex flex-col px-3 pb-3 gap-2">
      <div className="text-[11px] text-fg-mute break-all">{commandTaskStatusLabel(task)}{outputFile ? ` · ${outputFile}` : ''}</div>
      <pre
        ref={scrollRef}
        className="flex-1 min-h-0 overflow-auto rounded-md border border-border bg-surface-alt px-2.5 py-2 text-[11.5px] leading-[1.5] font-mono text-fg whitespace-pre-wrap break-all"
      >
        {output}
      </pre>
    </div>
  );
}

function SubagentTranscriptView({ task, workspaceHash = '', messageAutoCollapse }) {
  // workspaceHash 让历史请求带上 ?workspace=:子会话不在 daemon 内存里时
  // (Desktop 重启后、网状 agent 被换出)daemon 才知道去哪个工作区读盘,
  // 否则多工作区 daemon 只查自己的 cwd,返回 404 SESSION_NOT_FOUND。
  const sessionRef = useMemo(() => ({
    sessionId: task.id,
    workspaceHash,
    busy: task.status === SUBAGENT_TASK_STATUS.RUNNING,
    title: taskDisplayTitle(task),
  }), [task.id, task.status, task.title, task.summary, workspaceHash]);
  const transcript = useSessionTranscript(sessionRef, { live: 'auto' });
  // 子会话记录里的附件 blob_url 本来就在子会话 id 下;loader 走 request() 带 token。
  const attachmentApi = useMemo(() => createApi(sessionRef), [sessionRef]);
  const running = transcript.busy || task.status === SUBAGENT_TASK_STATUS.RUNNING;
  const items = useMemo(
    () => projectSubagentTranscriptItems(transcript.items, {
      messageAutoCollapse,
      deferTrailingToolSummary: running,
      ensureLiveActivity: running,
      liveTurnId: `subagent:${task.id}`,
    }),
    [messageAutoCollapse, running, task.id, transcript.items],
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
        <AttachmentTextLoaderContext.Provider value={attachmentApi.readAttachmentText}>
          <TranscriptItems
            items={items}
            messageAutoCollapse={messageAutoCollapse}
            capabilities={READ_ONLY_TRANSCRIPT_CAPABILITIES}
            expandedActivityKeys={expandedActivityKeys}
            collapsedMediaKeys={collapsedMediaKeys}
            onToggleActivity={toggleActivitySummary}
            onToggleMedia={toggleMediaGroup}
            sessionRunning={running}
            onReviewToggle={pauseTailFollowForReview}
          />
        </AttachmentTextLoaderContext.Provider>
        {transcript.loadState === 'loaded' && items.length === 0 && (
          <div className="text-[12px] text-fg-mute px-1 py-2">暂无会话内容</div>
        )}
      </div>
    </div>
  );
}

export function BackgroundTasksView({ tasks = [], taskId = '', onSelectTask, workspaceHash = '', onAbort, onStopCommand, readCommandOutput, messageAutoCollapse = true }) {
  const groups = useMemo(() => subagentTaskGroups(tasks), [tasks]);
  // 目标任务不在列表(如列表尚未刷新到它)时,合成一个最小任务对象,
  // transcript 仍能按 session_id 拉取展示。
  const transcriptTask = taskId
    ? (tasks.find((t) => t.id === taskId)
       || { id: taskId, status: SUBAGENT_TASK_STATUS.COMPLETED, title: '', summary: '' })
    : null;
  const openTask = (task) => onSelectTask?.(task.id);

  // 列表视图的时间刷新:有运行中任务时每秒走一次耗时;只剩已结束任务时放慢到 30 秒,
  // 只为更新「N 分钟前」(页签不激活时本组件不挂载)。
  const showingList = !transcriptTask;
  const [nowMs, setNowMs] = useState(() => Date.now());
  useEffect(() => {
    if (!showingList || tasks.length === 0) return undefined;
    setNowMs(Date.now());
    const timer = setInterval(() => setNowMs(Date.now()), groups.running.length > 0 ? 1000 : 30000);
    return () => clearInterval(timer);
  }, [showingList, tasks.length, groups.running.length]);

  const renderTask = (task) => (
    <TaskRow
      key={task.id}
      task={task}
      nowMs={nowMs}
      onStop={isCommandTask(task) ? (t) => onStopCommand?.(t.id) : onAbort}
      onOpen={openTask}
    />
  );

  return (
    <div className="flex-1 min-w-0 min-h-0 h-full flex flex-col bg-surface" data-background-tasks-view="true">
      {transcriptTask && (
        <div className="h-10 px-3 flex items-center gap-2 shrink-0">
          <button
            type="button"
            onClick={() => onSelectTask?.('')}
            className="w-7 h-7 rounded-md text-fg-mute flex items-center justify-center transition hover:bg-surface-hi hover:text-fg"
            title="返回任务列表"
            aria-label="返回任务列表"
          >
            <VsIcon name="arrowLeft" size={14} />
          </button>
          <span className="flex-1 min-w-0 text-[13px] font-semibold text-fg truncate">
            {isCommandTask(transcriptTask) ? transcriptTask.command : taskDisplayTitle(transcriptTask)}
          </span>
        </div>
      )}

      {transcriptTask && isCommandTask(transcriptTask) ? (
        <CommandOutputView key={transcriptTask.id} task={transcriptTask} readOutput={readCommandOutput} />
      ) : transcriptTask ? (
        <SubagentTranscriptView
          key={transcriptTask.id}
          task={transcriptTask}
          workspaceHash={workspaceHash}
          messageAutoCollapse={messageAutoCollapse}
        />
      ) : (
        <div className="flex-1 min-h-0 overflow-y-auto px-3 py-3 flex flex-col gap-3">
          {tasks.length === 0 && (
            <div className="text-[12px] text-fg-mute py-4 text-center">
              当前会话还没有后台任务。
            </div>
          )}
          {groups.running.length > 0 && (
            <div className="flex flex-col">
              <div className="mb-1 text-[11.5px] font-medium text-fg-2">运行中</div>
              {groups.running.map(renderTask)}
            </div>
          )}
          {groups.settled.length > 0 && (
            <div className="flex flex-col">
              <div className="mb-1 text-[11.5px] font-medium text-fg-2">
                已完成 {groups.settled.length}
              </div>
              {groups.settled.map(renderTask)}
            </div>
          )}
        </div>
      )}
    </div>
  );
}
