// ThemeContext + persist + system preference fallback。
//
// 主题切换不是改 inline style 而是改 <html data-theme="dark|light">,所有
// 颜色由 CSS variables 驱动(见 styles/globals.css)。
//
// 持久化走 lib/usePreference.js — 与 view / sidePanelCollapsed / layout widths
// 共享同一份 try/catch 兜底逻辑。useTheme() 对外 API 不变。

import { createContext, useCallback, useContext, useEffect } from 'react';
import { usePreference } from './lib/usePreference.js';
import { pushWindowBackgroundColor } from './lib/desktopWindowBackground.js';
import {
  effectiveAppearanceTheme,
  initialAppearancePreferences,
} from './lib/appearancePreferences.js';
import {
  COLOR_THEME_STORAGE_KEY,
  DEFAULT_COLOR_THEME,
  effectiveColorTheme,
  isValidColorTheme,
} from './lib/colorTheme.js';

const STORAGE_KEY = 'ace.theme';
const ThemeCtx = createContext({
  theme: 'light',
  colorTheme: DEFAULT_COLOR_THEME,
  toggle: () => {},
  set: () => {},
  setColorTheme: () => {},
});

function isValidTheme(v) { return v === 'light' || v === 'dark'; }

export function ThemeProvider({ children }) {
  // Desktop 会在模块执行前注入稳定配置;普通 WebUI 则以系统/默认值起步。
  // 当前 origin 的 localStorage 仍作为首帧缓存,认证后由 App 用 daemon 配置校正。
  const initialAppearance = initialAppearancePreferences();
  const [theme, setTheme] = usePreference(
    STORAGE_KEY,
    effectiveAppearanceTheme(initialAppearance.theme),
    isValidTheme,
  );
  const [colorTheme, setColorThemePreference] = usePreference(
    COLOR_THEME_STORAGE_KEY,
    initialAppearance.colorTheme,
    isValidColorTheme,
  );

  useEffect(() => {
    document.documentElement.setAttribute('data-theme', theme);
    document.documentElement.setAttribute('data-color-theme', colorTheme);
    // 两个主题维度落地后 --ace-bg 的 computed 值同步可读;把 body 底色推给
    // 桌面壳,native 换窗口打底色(快速 resize 的新暴露区域随主题,不闪黑/白)。
    // 非桌面壳环境内部 no-op。
    pushWindowBackgroundColor();
  }, [colorTheme, theme]);

  const toggle = useCallback(() => setTheme((t) => (t === 'dark' ? 'light' : 'dark')), [setTheme]);
  const set    = useCallback((t) => setTheme(t === 'dark' ? 'dark' : 'light'), [setTheme]);
  const setColorTheme = useCallback(
    (value) => setColorThemePreference(effectiveColorTheme(value)),
    [setColorThemePreference],
  );

  return (
    <ThemeCtx.Provider value={{ theme, colorTheme, toggle, set, setColorTheme }}>
      {children}
    </ThemeCtx.Provider>
  );
}

export function useTheme() { return useContext(ThemeCtx); }
