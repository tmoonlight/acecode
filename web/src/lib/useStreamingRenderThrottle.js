import { useEffect, useRef, useState } from 'react';

// 流式消息的 Markdown 重渲染间隔。每个 token 都全文重新解析、替换尾块 DOM,
// 会把 WebView 的 DOM 堆撑到高水位且不归还(实测 4 字符一个 token 时,
// 8 个长回合渲染进程比一次性输出多涨约 90 MB);100ms 一次观感上仍是连续出字。
export const STREAMING_RENDER_INTERVAL_MS = 100;

// 距上次刷新还要等多久才能再刷新;0 = 立即。lastRenderAt 为 0 表示还没刷新过,
// 第一段文字不等待。
export function streamingRenderDelay(lastRenderAt, now, interval = STREAMING_RENDER_INTERVAL_MS) {
  if (!(lastRenderAt > 0)) return 0;
  return Math.max(0, lastRenderAt + interval - now);
}

// 流式期间返回按 interval 节流后的文本(首段立即、之后每个间隔最多一次、
// 最后一段必定送达);不在流式时原样返回,结束的那一刻立即显示完整内容。
export function useStreamingRenderThrottle(value, active, interval = STREAMING_RENDER_INTERVAL_MS) {
  const [shown, setShown] = useState(value);
  const lastRenderAtRef = useRef(0);
  useEffect(() => {
    if (!active) {
      lastRenderAtRef.current = 0;
      if (shown !== value) setShown(value);
      return undefined;
    }
    if (shown === value) return undefined;
    // 每个新 token 都会重设计时器,但截止时间按上次刷新时刻算,不会被持续的
    // token 无限推迟。
    const timer = setTimeout(() => {
      lastRenderAtRef.current = Date.now();
      setShown(value);
    }, streamingRenderDelay(lastRenderAtRef.current, Date.now(), interval));
    return () => clearTimeout(timer);
  }, [value, active, shown, interval]);
  return active ? shown : value;
}
