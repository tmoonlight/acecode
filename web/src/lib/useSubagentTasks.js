// 后台任务(spawn_subagent 子会话)的数据 hook。挂在 ChatView(主会话视图)。
//
// 职责:
//   1. REST GET /api/sessions?parent=<id> 拉任务快照(parent 切换时重置)。
//   2. 监听 connection 消息:父会话的 spawn_subagent tool_end → refetch;
//      子会话事件 → applySubagentSessionEvent 增量(纯函数,见 subagentTasks.js)。
//   3. 对运行中的子任务保持 WS 订阅(connection.retainSession)——
//      **不依赖面板是否打开**:这是子会话 permission_request / question_request
//      能到达 App 全局监听并冒泡到主会话 UI 的前提。已结束任务不订阅。
//   4. 操作:只有中止(sendAbort + 本地标记)。子会话没有单独的归档 / 删除:
//      它们始终跟随主会话,只在主会话归档后被永久删除时一起删除(daemon 级联)。

import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from './api.js';
import { connection } from './connection.js';
import {
  SUBAGENT_TASK_STATUS,
  applySubagentSessionEvent,
  isSubagentSpawnStartEvent,
  markSubagentTaskAborted,
  mergeSubagentTaskList,
  runningSubagentCount,
  shouldRefreshSubagentTasksFromStatus,
} from './subagentTasks.js';

export function useSubagentTasks(parentSessionId, { onSpawnStart } = {}) {
  const [tasks, setTasks] = useState([]);
  const retainedRef = useRef(new Set());
  const parentRef = useRef(parentSessionId);
  useEffect(() => { parentRef.current = parentSessionId; }, [parentSessionId]);
  const onSpawnStartRef = useRef(onSpawnStart);
  useEffect(() => { onSpawnStartRef.current = onSpawnStart; }, [onSpawnStart]);
  const taskIdsRef = useRef(new Set());
  useEffect(() => { taskIdsRef.current = new Set(tasks.map((t) => t.id)); }, [tasks]);

  const refresh = useCallback(async () => {
    if (!parentSessionId) return;
    try {
      const sessions = await api.listSessions({ parent: parentSessionId });
      // 迟到响应守卫:切换主会话后,旧 parent 的响应直接丢弃。
      if (parentRef.current !== parentSessionId) return;
      setTasks((prev) => mergeSubagentTaskList(prev, Array.isArray(sessions) ? sessions : []));
    } catch {
      // 静默:列表拉取失败不打断主会话;下一次事件/打开面板会重试。
    }
  }, [parentSessionId]);

  useEffect(() => {
    setTasks([]);
    if (!parentSessionId) return;
    refresh();
  }, [parentSessionId, refresh]);

  useEffect(() => {
    if (!parentSessionId) return undefined;
    const handler = (event) => {
      const msg = event.detail || {};
      const sid = msg.session_id || msg.payload?.session_id || '';
      if (sid === parentSessionId) {
        if (isSubagentSpawnStartEvent(parentSessionId, msg)) {
          onSpawnStartRef.current?.(msg);
        }
        if (msg.type === 'tool_end') {
          const p = msg.payload || {};
          if (((p.tool === 'spawn_subagent' || p.tool === 'agent_spawn') &&
               p.metadata?.subagent_session_id) ||
              p.tool === 'wait_subagent' || p.tool === 'agent_wait') {
            refresh();
          }
        }
        return;
      }
      // spawn_subagent(wait=true) 期间父 turn 还没产生 tool_end。父订阅会收到
      // 带 parent_session_id 的子会话 session_status;只有明确归属当前 parent
      // 的未知 child 才触发 refetch,避免工作区内无关 busy 会话造成盲刷新。
      if (msg.type === 'session_status') {
        const p = msg.payload || {};
        const statusParentId = p.parent_session_id || msg.parent_session_id || '';
        if (shouldRefreshSubagentTasksFromStatus(
          parentSessionId,
          taskIdsRef.current,
          msg,
        )) {
          refresh();
          return;
        }
        if (statusParentId && statusParentId !== parentSessionId) return;
      }
      setTasks((prev) => applySubagentSessionEvent(prev, msg));
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, [parentSessionId, refresh]);

  // 运行中任务的订阅管理:目标集合 = running task ids,与已 retain 集合 diff。
  useEffect(() => {
    const target = new Set(
      tasks.filter((t) => t.status === SUBAGENT_TASK_STATUS.RUNNING).map((t) => t.id));
    const retained = retainedRef.current;
    for (const id of target) {
      if (!retained.has(id)) {
        connection.retainSession(id);
        retained.add(id);
      }
    }
    for (const id of [...retained]) {
      if (!target.has(id)) {
        connection.releaseSession(id);
        retained.delete(id);
      }
    }
  }, [tasks]);

  // 卸载 / parent 切换:释放全部订阅。
  useEffect(() => () => {
    for (const id of retainedRef.current) connection.releaseSession(id);
    retainedRef.current = new Set();
  }, [parentSessionId]);

  const abortTask = useCallback((id) => {
    if (!id) return;
    if (!connection.sendAbort(id)) return false;
    setTasks((prev) => markSubagentTaskAborted(prev, id));
    return true;
  }, []);

  useEffect(() => {
    const onDisconnect = () => setTasks((prev) => prev.map((task) => task.abortPending
      ? { ...task, abortPending: false } : task));
    connection.addEventListener('disconnect', onDisconnect);
    return () => connection.removeEventListener('disconnect', onDisconnect);
  }, []);

  return {
    tasks,
    runningCount: runningSubagentCount(tasks),
    refresh,
    abortTask,
  };
}
