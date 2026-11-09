import { useEffect, useState } from 'react';
import { NARROW_LAYOUT_QUERY } from './mobileShell.js';

// 当前视口是否窄于 600(手机竖屏)。折叠屏展开 / 合上、旋转屏幕时实时更新。
export function useNarrowLayout() {
  const [narrow, setNarrow] = useState(() => (
    typeof window !== 'undefined' && typeof window.matchMedia === 'function'
      ? window.matchMedia(NARROW_LAYOUT_QUERY).matches
      : false
  ));
  useEffect(() => {
    if (typeof window.matchMedia !== 'function') return undefined;
    const query = window.matchMedia(NARROW_LAYOUT_QUERY);
    const update = () => setNarrow(query.matches);
    update();
    query.addEventListener('change', update);
    return () => query.removeEventListener('change', update);
  }, []);
  return narrow;
}
