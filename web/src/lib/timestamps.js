export function parseTimestampMs(value) {
  if (value === undefined || value === null || value === '') return 0;
  if (typeof value === 'number') {
    return Number.isFinite(value) && value > 0 ? value : 0;
  }
  const numeric = Number(value);
  if (Number.isFinite(numeric) && numeric > 0) return numeric;
  const parsed = Date.parse(String(value));
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 0;
}

// 历史消息推断出来的时间放在这个私有字段里,只作最后的兜底,不冒充服务端字段。
export const INFERRED_TIMESTAMP_KEY = '__inferredTimestampMs';

export function transcriptTimestampMs(item) {
  return parseTimestampMs(item?.ts ?? item?.timestamp_ms ?? item?.timestampMs ?? item?.timestamp)
    || parseTimestampMs(item?.[INFERRED_TIMESTAMP_KEY]);
}

function isTurnBoundaryMessage(message) {
  return message?.role === 'user' && !message?.is_meta;
}

// 落盘时只有 user / system / error 行带 timestamp;assistant / tool 行的 id 由
// role + content + 空 timestamp 哈希得出(实时派发与重读 JSONL 要算出同一个 id),
// 所以它们没有自己的时间。历史加载时拿同一回合里下一条带时间的消息顶上:紧跟着的
// 文件检查点、[Turn net diff] / [Turn timing] 行就是这段输出完成的时刻。遇到下一条
// 真实 user 行即回合边界,不往后借(下一回合可能隔了几天);回合还没结束或这一页
// 到头时,退回前一条带时间的消息。都找不到保持 0,由调用方兜底。
export function inferHistoryTimestamps(messages) {
  const list = Array.isArray(messages) ? messages : [];
  const inferred = new Array(list.length).fill(0);
  let later = 0;
  for (let i = list.length - 1; i >= 0; i -= 1) {
    const own = transcriptTimestampMs(list[i]);
    if (own) later = own;
    inferred[i] = own || later;
    if (isTurnBoundaryMessage(list[i])) later = 0;
  }
  let earlier = 0;
  for (let i = 0; i < list.length; i += 1) {
    const own = transcriptTimestampMs(list[i]);
    if (own) earlier = own;
    else if (!inferred[i]) inferred[i] = earlier;
  }
  return inferred;
}

export function withInferredHistoryTimestamps(messages) {
  const list = Array.isArray(messages) ? messages : [];
  const inferred = inferHistoryTimestamps(list);
  return list.map((message, index) => (
    message && typeof message === 'object' && !transcriptTimestampMs(message) && inferred[index]
      ? { ...message, [INFERRED_TIMESTAMP_KEY]: inferred[index] }
      : message
  ));
}
