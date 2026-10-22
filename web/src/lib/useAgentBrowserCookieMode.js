import { useEffect, useSyncExternalStore } from 'react';
import {
  agentBrowserCookieModeStore,
  installAgentBrowserCookieModeListener,
  loadAgentBrowserCookieMode,
} from './agentBrowserCookieMode.js';

// 工具栏「更多」菜单与「设置 > Agent 浏览器」共用的档位状态。挂载时向 Desktop 读一次
// 当前档位,并跟随 Desktop 的切档事件;写入只经 setAgentBrowserCookieMode。
export function useAgentBrowserCookieMode() {
  const state = useSyncExternalStore(
    agentBrowserCookieModeStore.subscribe,
    agentBrowserCookieModeStore.getState,
  );
  useEffect(() => {
    void loadAgentBrowserCookieMode();
    return installAgentBrowserCookieModeListener();
  }, []);
  return state;
}
