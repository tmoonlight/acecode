import { useCallback, useEffect, useLayoutEffect, useRef, useState, useSyncExternalStore } from 'react';
import { createSessionTitleAnimationStore, sessionTitleAnimationKey } from './sessionTitleAnimation.js';

const reducedMotionQuery = '(prefers-reduced-motion: reduce)';
const store = createSessionTitleAnimationStore({
  now: () => performance.now(),
  schedule: (tick) => requestAnimationFrame(tick),
  cancel: (id) => cancelAnimationFrame(id),
  shouldAnimate: () => !document.hidden,
  reducedMotion: () => window.matchMedia?.(reducedMotionQuery).matches === true,
});
const serverSnapshot = () => null;

export function useSessionTitleAnimation(session, title, ready = true) {
  const key = sessionTitleAnimationKey(session);
  const previous = useRef({ key, title, ready });
  const [target, setTarget] = useState(null);
  const subscribe = useCallback((listener) => store.subscribe(key, listener), [key]);
  const getSnapshot = useCallback(() => store.getSnapshot(key), [key]);
  const snapshot = useSyncExternalStore(subscribe, getSnapshot, serverSnapshot);

  useLayoutEffect(() => {
    const before = previous.current;
    previous.current = { key, title, ready };
    if (ready && before.ready && key === before.key && title !== before.title) {
      setTarget({ key, title });
      store.start(key, title);
    } else if (!ready || !before.ready || key !== before.key) {
      setTarget(null);
    }
  }, [key, title, ready]);

  const animation = ready && target?.key === key && target?.title === title && snapshot?.title === title
    ? snapshot : null;
  const typing = Boolean(animation?.typing);
  useEffect(() => {
    if (!typing) return undefined;
    const finishIfHidden = () => {
      if (document.hidden) store.finish(key);
    };
    finishIfHidden();
    document.addEventListener('visibilitychange', finishIfHidden);
    return () => {
      document.removeEventListener('visibilitychange', finishIfHidden);
    };
  }, [key, typing]);
  return animation;
}
