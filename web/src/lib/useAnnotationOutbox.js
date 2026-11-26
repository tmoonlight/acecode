import { useSyncExternalStore } from 'react';

const EMPTY_SNAPSHOT = Object.freeze({
  revision: 0, records: Object.freeze([]), pages: Object.freeze([]), pendingCount: 0,
});

// 待发批注箱的订阅入口。ChatView 不直接写依赖会变的 useCallback，
// 订阅/取快照的闭包固定在 hook 里，随 box 变化整体替换。
export function useAnnotationOutbox(box) {
  return useSyncExternalStore(
    box ? (listener) => box.subscribe(listener) : () => {},
    box ? () => box.getSnapshot() : () => EMPTY_SNAPSHOT,
  );
}
