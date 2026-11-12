// 「调用了 N 个智能体」分组卡片(spawn_subagent / wait_subagent 的聚合渲染)。
//
// 折叠:一行「调用了 N 个智能体」+ 展开箭头(样式对齐 ActivitySummaryBlock),
//       行首是这几个智能体的头像(每个子代理一张固定的像素小人)。
// 展开:逐个智能体标题行,点击 → onOpen(sessionId) 打开后台任务面板并定位到
//       该子会话的 transcript(即「后台任务」+「查看会话」的效果)。
//
// 标题解析优先级:后台任务列表实时标题(taskDisplayTitle)> spawn 时的 prompt
// 摘要 > 兜底「智能体」。运行 / 完成 / 中止画在头像右上角,只认任务列表里的状态。

import { useState } from 'react';
import { clsx } from '../lib/format.js';
import {
  SUBAGENT_TASK_STATUS,
  subagentAvatarStatus,
  subagentGroupAvatars,
  taskDisplayTitle,
} from '../lib/subagentTasks.js';
import { ActivityLine } from './ActivityLine.jsx';
import { VsIcon } from './Icon.jsx';
import { SubagentAvatar, SubagentAvatars } from './PixelAvatar.jsx';

function agentTitle(agent, task) {
  if (task) {
    const t = taskDisplayTitle(task);
    if (t) return t;
  }
  if (agent.prompt) return agent.prompt;
  return '智能体';
}

function AgentRow({ agent, task, onOpen }) {
  const title = agentTitle(agent, task);
  const clickable = !!agent.sessionId;
  return (
    <button
      type="button"
      disabled={!clickable}
      onClick={() => clickable && onOpen?.(agent.sessionId)}
      title={clickable ? '查看子会话' : '子会话尚未就绪'}
      className={clsx(
        'group/agent w-full flex items-center gap-2.5 px-2 py-1 rounded-md text-left transition',
        clickable ? 'hover:bg-surface-hi cursor-pointer' : 'cursor-default opacity-70',
      )}
    >
      <SubagentAvatar sessionId={agent.sessionId} expert={task?.expert} status={subagentAvatarStatus(task)} />
      <span className="flex-1 min-w-0 truncate text-[13px] text-fg">{title}</span>
      {clickable && (
        <VsIcon
          name="arrowRight"
          size={12}
          className="shrink-0 text-fg-mute opacity-0 group-hover/agent:opacity-100 transition-opacity"
        />
      )}
    </button>
  );
}

export function SubagentGroupBlock({ agents = [], tasksById, onOpen, messageAutoCollapse = true }) {
  const [manuallyExpanded, setExpanded] = useState(false);
  const expanded = !messageAutoCollapse || manuallyExpanded;
  const count = agents.length;
  const byId = tasksById || new Map();
  // 回合结束后子会话还在跑(后台子代理)→ 这一行带炫光,与包着它的「已处理」一致。
  // 只认任务列表里的状态:没查到任务时不靠工具项的 done 标记猜,被中断的调用不会一直亮。
  const anyRunning = agents.some((agent) => (
    agent.sessionId && byId.get(agent.sessionId)?.status === SUBAGENT_TASK_STATUS.RUNNING
  ));
  const avatars = subagentGroupAvatars(agents, byId);

  return (
    <div className="min-w-0">
      <ActivityLine
        icon={<VsIcon name="embedding" size={13} className="opacity-80" />}
        leading={avatars.length > 0 ? <SubagentAvatars avatars={avatars} /> : null}
        label={`调用了 ${count} 个智能体`}
        live={anyRunning}
        expandable={messageAutoCollapse}
        expanded={expanded}
        onToggle={messageAutoCollapse ? () => setExpanded((v) => !v) : undefined}
        title={messageAutoCollapse ? (expanded ? '收起智能体' : '展开智能体') : undefined}
        ariaLabel={messageAutoCollapse ? (expanded ? '收起智能体' : '展开智能体') : undefined}
      />
      {expanded && (
        <div className="mt-1 flex flex-col gap-0.5">
          {agents.map((agent) => (
            <AgentRow
              key={agent.sessionId || agent.itemId}
              agent={agent}
              task={agent.sessionId ? byId.get(agent.sessionId) : null}
              onOpen={onOpen}
            />
          ))}
        </div>
      )}
    </div>
  );
}
