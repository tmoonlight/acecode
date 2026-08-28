// 侧边栏每轮 refresh 都会对 workspace 做两次扇出:每个 workspace 一次
// pinned-sessions、一次 opencode-import。14 个 workspace 就是 28 个请求,而
// refresh 由 5 秒定时器驱动 —— 浏览器对同一域名只有 6 条并发连接,这些请求
// 把管道占满后,用户点击展开触发的那个会话列表请求只能排队,表现为侧边栏
// 长时间停在「加载中...」(实测排队 430ms,而服务端处理只要 9~16ms)。
//
// 这两类数据都不需要每轮对全部 workspace 重取:
//   - pinned-sessions 只在该 workspace 的会话列表可见时才会被渲染;
//   - opencode-import 探测的是「这个目录有没有 opencode 数据可导入」,
//     是近乎静态的事实,首轮探一次即可。
//
// 下面两个纯函数决定每一轮真正要请求哪些 workspace,逻辑与 React 解耦以便
// 直接单测。

function normalizeHash(value) {
  return String(value || '').trim();
}

function toHashSet(values) {
  const out = new Set();
  for (const value of values || []) {
    const hash = normalizeHash(typeof value === 'string' ? value : value?.hash);
    if (hash) out.add(hash);
  }
  return out;
}

/**
 * 每轮该重取 pinned-sessions 的 workspace。
 *
 * 只取「可见」的那些(active / 展开 / reveal 目标 / __local__)。折叠的
 * workspace 沿用上一轮缓存值 —— 调用方对不在返回集合里的 hash 必须保留旧值,
 * 而不是清空,否则展开时会短暂丢失置顶标记。
 */
export function pinnedRefreshTargets(workspaces = [], visibleHashes = []) {
  const visible = toHashSet(visibleHashes);
  const out = [];
  for (const workspace of workspaces || []) {
    const hash = normalizeHash(workspace?.hash);
    if (!hash) continue;
    if (visible.has(hash)) out.push(hash);
  }
  return out;
}

/**
 * 每轮该探测 opencode-import 的 workspace。
 *
 * 没探过的探一次(首轮会把全部 workspace 覆盖到,保证提示不丢),之后只在
 * 可见的 workspace 上重探。`__local__` 没有对应目录,永远跳过。
 */
export function opencodePreviewTargets(workspaces = [], visibleHashes = [], alreadyProbed = []) {
  const visible = toHashSet(visibleHashes);
  const probed = toHashSet(alreadyProbed);
  const out = [];
  for (const workspace of workspaces || []) {
    const hash = normalizeHash(workspace?.hash);
    if (!hash || hash === '__local__') continue;
    if (!probed.has(hash) || visible.has(hash)) out.push(hash);
  }
  return out;
}
