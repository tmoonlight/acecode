import { useMemo, useSyncExternalStore } from 'react';

const EMPTY_SNAPSHOT = Object.freeze({
  revision: 0, records: Object.freeze([]), pages: Object.freeze([]), pendingCount: 0,
});
const subscribeToNothing = () => () => {};
const emptySnapshot = () => EMPTY_SNAPSHOT;

// 待发批注箱的订阅入口。ChatView 不直接写依赖会变的 useCallback，
// 订阅/取快照的函数固定在这个 hook 里，只在 box 换了的时候才换。
// 订阅函数每次渲染都换身份的话，useSyncExternalStore 每次渲染都会退订再重订。
export function useAnnotationOutbox(box) {
  const store = useMemo(() => (box
    ? { subscribe: (listener) => box.subscribe(listener), getSnapshot: () => box.getSnapshot() }
    : { subscribe: subscribeToNothing, getSnapshot: emptySnapshot }), [box]);
  return useSyncExternalStore(store.subscribe, store.getSnapshot);
}
