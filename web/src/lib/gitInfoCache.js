// `/api/git/info` 的**共享**缓存实例(单例),与 gitChangesCache.js 同构。
//
// 为什么必须共享:该端点在 daemon 侧要 spawn 5~7 个 git 子进程
//(rev-parse / symbolic-ref / show-ref ×N / for-each-ref / status --porcelain),
// Windows 上光进程启动就上百毫秒,大仓库 status 更久。之前只有 Sidebar 的
// 会话 hover 卡片带 30s TTL 缓存,GitSessionPill **完全没有缓存**,而它挂在
// ChatView 上且 key 含 sid —— 每次切会话都整组件重挂、无条件重拉一次。
// 用户在会话间来回切时,Fiddler 里看到的就是 /api/git/info 高频且经常挂起
//(请求排在 Crow 工作线程池后面)。
//
// 同一 workspace 下所有会话共用一个 cwd,所以按 cwd 缓存的命中率极高;
// 缓存本身还带在途请求去重(同 cwd 并发只发一次)。
//
// 失效:监听 GIT_STATE_CHANGED_EVENT(checkout / worktree 等外部 git 状态
// 变更后由 GitSessionPill 广播),整条 cwd 清掉,下次读取重拉。

import { api } from './api.js';
import { createSessionHoverGitInfoCache } from './sessionHoverDetails.js';
import { GIT_STATE_CHANGED_EVENT } from './gitSessionPill.js';

// 工厂名字带 sessionHover 是历史包袱(最早只服务 hover 卡片),行为是通用的
// 「按 key 的 TTL + 在途去重」缓存,这里直接复用,不改它的名字以免动它的单测。
export const gitInfoCache = createSessionHoverGitInfoCache((cwd) => api.gitInfo(cwd));

if (typeof window !== 'undefined') {
  window.addEventListener(GIT_STATE_CHANGED_EVENT, (event) => {
    const changedCwd = String(event?.detail?.cwd || '');
    // detail 不带 cwd 时保守起见整表清空。
    gitInfoCache.invalidate(changedCwd);
  });
}
