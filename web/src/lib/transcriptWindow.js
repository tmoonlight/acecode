// Transcript 尾部窗口(渐进虚拟化)。
//
// 大会话(数百条消息 × markdown/highlight/diff2html)一次性全量渲染会把
// webview 主线程同步烧几秒 —— feedback IQSZ-D0668 的 478 条 / 1.5MB 会话
// 切进去就是秒级冻结。完整的绝对定位虚拟化会打碎滚动锚定、`.ace-md-block`
// 前缀 memo 与行高测量,代价不成比例;这里采用尾部窗口:初始只渲染最近
// INITIAL_TAIL_ITEMS 条投影行,更早的行折叠成一个「显示更早消息」入口,
// 按块揭示或一键全量。
//
// 锚点语义:窗口用**首个可见行的稳定 key** 锚定,而不是数量 ——
//   - 持久化 messageId 优先,并与 reducer 临时 item id 分别命名空间,
//     因此 transcript_replace 重建 item id 时仍能定位同一消息;
//   - 流式追加发生在尾部,锚点不动,窗口自然增长,用户正在读的行不会
//     因为新消息到达而被顶出窗口;
//   - 锚点只选 user 行(user 行不参与活动折叠),窗口边界落在回合边界
//     上,首行永远是一条完整的用户消息;
//   - 底层切片在锚点找不到时仍 fail-open 成全量渲染,但 ChatView 会先
//     同步协调失效 key,正常主路径不会经历全量帧。
//
// 所有函数均为纯函数,Node 单测在 transcriptWindow.test.js。

export const INITIAL_TAIL_ITEMS = 120;
export const REVEAL_CHUNK_ITEMS = 240;

function isUserRow(item) {
  return !!item && item.kind === 'msg' && item.role === 'user';
}

function identityText(value) {
  if (value === undefined || value === null) return '';
  const text = String(value);
  return text.trim() ? text : '';
}

// messageId 与临时 item id 必须带命名空间,避免字面值相同时误匹配。
export function transcriptWindowItemKey(item) {
  const messageId = identityText(item?.messageId);
  if (messageId) return `message:${messageId}`;
  const itemId = identityText(item?.id);
  return itemId ? `item:${itemId}` : null;
}

function anchorIndex(items, anchorKey) {
  if (!anchorKey) return -1;
  return items.findIndex((item) => transcriptWindowItemKey(item) === anchorKey);
}

// 从 boundary 向前找最近的 user 行作为锚;找不到(如单个超长回合)退回
// boundary 行自身。返回 null = 不需要窗口(条目太少)。
export function initialWindowAnchorKey(items, tailCount = INITIAL_TAIL_ITEMS) {
  const list = Array.isArray(items) ? items : [];
  if (list.length <= tailCount) return null;
  const boundary = list.length - tailCount;
  for (let i = boundary; i >= 0; i -= 1) {
    if (isUserRow(list[i])) {
      if (i === 0) return null;
      const key = transcriptWindowItemKey(list[i]);
      if (key) return key;
    }
  }
  return transcriptWindowItemKey(list[boundary]);
}

// 在当前 render 切片前协调窗口状态:
// undefined = 尚未初始化 / 空 transcript;null = 显式全量视图。
export function reconcileTranscriptWindowAnchorKey(
  items,
  anchorKey,
  tailCount = INITIAL_TAIL_ITEMS,
) {
  const list = Array.isArray(items) ? items : [];
  if (list.length === 0) return undefined;
  if (list.length <= tailCount) return null;
  if (anchorKey === null) return null;
  if (typeof anchorKey === 'string' && anchorIndex(list, anchorKey) >= 0) {
    return anchorKey;
  }
  return initialWindowAnchorKey(list, tailCount);
}

// 按锚点切窗口。anchorKey 为空或找不到 → 全量(fail-open)。
export function windowTranscriptItems(items, anchorKey) {
  const list = Array.isArray(items) ? items : [];
  const idx = anchorIndex(list, anchorKey);
  if (idx <= 0) return { visible: list, hiddenCount: 0 };
  return { visible: list.slice(idx), hiddenCount: idx };
}

// 「显示更早」:锚点向前挪 chunk 条,再对齐到最近的 user 行。
// 返回 null = 剩余部分不多了,直接全量。
export function revealEarlierAnchorKey(items, anchorKey, chunk = REVEAL_CHUNK_ITEMS) {
  const list = Array.isArray(items) ? items : [];
  const idx = anchorIndex(list, anchorKey);
  if (idx <= 0) return null;
  const target = idx - chunk;
  if (target <= 0) return null;
  for (let i = target; i >= 0; i -= 1) {
    if (isUserRow(list[i])) {
      if (i === 0) return null;
      const key = transcriptWindowItemKey(list[i]);
      if (key) return key;
    }
  }
  return null;
}
