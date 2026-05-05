// 9 宫格视图:当前 workspace 最多 8 条会话 + 1 个"新对话"格,空位填充"空闲"。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { MiniSession } from './MiniSession.jsx';

export function Grid9View({ activeRef, onExpand, onOpenHome }) {
  const [sessions, setSessions] = useState([]);

  useEffect(() => {
    let off = false;
    const tick = () => {
      const req = activeRef?.workspaceHash
        ? api.listWorkspaceSessions(activeRef.workspaceHash)
        : api.listSessions();
      req
        .then((list) => { if (!off) setSessions(Array.isArray(list) ? list.slice(0, 8) : []); })
        .catch(() => {});
    };
    tick();
    const t = setInterval(tick, 3000);
    return () => { off = true; clearInterval(t); };
  }, [activeRef?.workspaceHash]);

  const slotsAfter = Math.max(0, 8 - sessions.length); // 留 1 格给"+ 新建"

  return (
    <div className="flex-1 overflow-hidden bg-bg p-1 grid grid-cols-3 grid-rows-3 gap-1">
      {sessions.map((s) => (
        <MiniSession key={s.id} session={s} compact onClick={onExpand} />
      ))}
      {Array.from({ length: slotsAfter }).map((_, i) => (
        <div
          key={`empty-${i}`}
          className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt flex items-center justify-center text-fg-mute text-[11px]"
        >
          空闲
        </div>
      ))}
      <button
        type="button"
        onClick={() => onOpenHome?.(activeRef)}
        className="rounded-lg border-[1.5px] border-dashed border-border bg-surface-alt flex flex-col items-center justify-center gap-1 text-fg-mute hover:border-accent hover:bg-accent-bg hover:text-accent transition"
      >
        <span className="text-2xl">+</span>
        <span className="text-[11px]">新对话</span>
      </button>
    </div>
  );
}
