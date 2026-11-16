// 桌面版「启动时回到上次停留的会话」的前端一半。
//
// 当前会话每次变化都经 aceDesktop_setLastSession 报给桌面壳(记在 state.json),
// 下次启动由桌面壳拼成 ?open=<会话>&restore=1 落地参数,走已有的启动跳转恢复。
// 停在首页 / 定时任务 / 专家组件等非会话页面时上报 null,记录随之清掉。
//
// 只有桌面壳有这个 bridge;浏览器直连 daemon 时这里什么都不做。
import {
  sessionJumpId,
  sessionJumpNoWorkspace,
  sessionJumpReadOnly,
  sessionJumpWorkspaceHash,
} from './sessionJump.js';

export function lastSessionRecordFromRef(ref) {
  if (!ref || typeof ref !== 'object') return null;
  if (ref.home || ref.loop || ref.expertComponents) return null;
  const sessionId = sessionJumpId(ref);
  if (!sessionId) return null;
  const record = { session_id: sessionId };
  if (sessionJumpNoWorkspace(ref)) {
    record.no_workspace = true;
  } else {
    const workspaceHash = sessionJumpWorkspaceHash(ref);
    if (workspaceHash) record.workspace_hash = workspaceHash;
  }
  if (sessionJumpReadOnly(ref)) record.read_only = true;
  return record;
}

// 同一条记录只上报一次:activeRef 会因为标题、模型等字段更新而换对象,
// 这些变化不该各触发一次 state.json 写入。
export function createLastSessionRecorder(
  win = typeof window !== 'undefined' ? window : undefined,
) {
  let lastKey;
  return {
    remember(ref) {
      const bridge = win?.aceDesktop_setLastSession;
      if (typeof bridge !== 'function') return false;
      const record = lastSessionRecordFromRef(ref);
      const key = JSON.stringify(record);
      if (key === lastKey) return false;
      lastKey = key;
      try {
        Promise.resolve(bridge(record)).catch(() => {
          // 记不住只影响下次启动落在哪;允许下一次变化重试。
          if (lastKey === key) lastKey = undefined;
        });
      } catch {
        lastKey = undefined;
      }
      return true;
    },
  };
}
