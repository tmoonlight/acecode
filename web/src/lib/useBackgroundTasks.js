import { useEffect, useMemo, useRef, useState } from 'react';
import { connection } from './connection.js';
import {
  applyBackgroundTaskEvent,
  markCommandTaskStopping,
  mergeCommandTaskList,
} from './backgroundTasks.js';
import { SUBAGENT_TASK_STATUS } from './subagentTasks.js';

// 当前会话的后台命令:REST 快照打底,background_task 事件增量更新。
// 返回的 stop / moveToBackground 引用稳定(内部读 ref),可直接传给子组件。
export function useBackgroundTasks(sessionId, apiClient) {
  const [tasks, setTasks] = useState([]);
  const latest = useRef({ sessionId, apiClient });
  latest.current = { sessionId, apiClient };

  useEffect(() => {
    setTasks([]);
    if (!sessionId || !apiClient?.listBackgroundTasks) return undefined;
    let cancelled = false;
    apiClient.listBackgroundTasks(sessionId)
      .then((response) => {
        if (cancelled) return;
        setTasks((previous) => mergeCommandTaskList(previous, response?.tasks));
      })
      .catch(() => {});
    return () => { cancelled = true; };
  }, [sessionId, apiClient]);

  useEffect(() => {
    if (!sessionId) return undefined;
    const handler = (event) => {
      const msg = event.detail || {};
      if (msg.type !== 'background_task') return;
      const eventSession = msg.session_id || msg.payload?.session_id || '';
      if (eventSession && eventSession !== sessionId) return;
      setTasks((previous) => applyBackgroundTaskEvent(previous, msg.payload?.task));
    };
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, [sessionId]);

  const actions = useMemo(() => ({
    stop: async (taskId) => {
      const { sessionId: sid, apiClient: client } = latest.current;
      if (!sid || !taskId || !client?.stopBackgroundTask) return false;
      setTasks((previous) => markCommandTaskStopping(previous, taskId));
      try {
        await client.stopBackgroundTask(sid, taskId);
        return true;
      } catch {
        return false;
      }
    },
    moveToBackground: async () => {
      const { sessionId: sid, apiClient: client } = latest.current;
      if (!sid || !client?.moveToBackground) return false;
      try {
        await client.moveToBackground(sid);
        return true;
      } catch {
        return false;
      }
    },
    readOutput: async (taskId) => {
      const { sessionId: sid, apiClient: client } = latest.current;
      if (!sid || !taskId || !client?.backgroundTaskOutput) return null;
      try {
        return await client.backgroundTaskOutput(sid, taskId);
      } catch {
        return null;
      }
    },
  }), []);

  const runningCount = tasks.reduce(
    (count, task) => count + (task.status === SUBAGENT_TASK_STATUS.RUNNING ? 1 : 0), 0);
  return { tasks, runningCount, ...actions };
}
