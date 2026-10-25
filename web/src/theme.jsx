// ThemeContext + persist + system preference fallback。
//
// 主题切换不是改 inline style 而是改 <html data-theme="dark|light">,所有
// 颜色由 CSS variables 驱动(见 styles/globals.css)。
//
// 持久化走 lib/usePreference.js — 与 view / sidePanelCollapsed / layout widths
// 共享同一份 try/catch 兜底逻辑。useTheme() 对外 API 不变。

import { createContext, useCallback, useContext, useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react';
import { usePreference } from './lib/usePreference.js';
import { api } from './lib/api.js';
import {
  EVA_THEME_ID, applyInstalledTheme, loadThemeResources, releaseThemeResource, resolveThemeVariant,
  revokeThemeResources, resolveThemeAppearance, selectThemeVariant, themeChromeStyle, themeSoundStyle,
  themeVariantOptions, themeWindowShape, validThemeDefinition,
} from './lib/themePackages.js';
import { pushWindowBackgroundColor } from './lib/desktopWindowBackground.js';
import { pushWindowShape } from './lib/desktopWindowShape.js';
import { desktopTaskbarBadge } from './lib/desktopTaskbarBadge.js';
import { HudEffects } from './components/HudEffects.jsx';
import { ThemeSoundLayer } from './components/ThemeSoundLayer.jsx';
import {
  effectiveAppearanceTheme,
  initialAppearancePreferences,
  validThemeVariantId,
} from './lib/appearancePreferences.js';
import {
  COLOR_THEME_STORAGE_KEY,
  DEFAULT_COLOR_THEME,
  effectiveColorTheme,
  isInstalledColorTheme,
  isValidColorTheme,
} from './lib/colorTheme.js';

const STORAGE_KEY = 'ace.theme';
const THEME_VARIANT_STORAGE_KEY = 'ace.themeVariant';
const THEME_EFFECTS_STORAGE_KEY = 'ace.themeEffects';
const THEME_SOUNDS_STORAGE_KEY = 'ace.themeSounds';
const NO_VARIANTS = Object.freeze([]);
const ThemeCtx = createContext({
  theme: 'light',
  colorTheme: DEFAULT_COLOR_THEME,
  appearance: null,
  themeVariant: '',
  themeEffects: true,
  themeSounds: true,
  themeChrome: null,
  themeSoundStyle: null,
  themeVariantOptions: NO_VARIANTS,
  activeThemeVariant: null,
  toggle: () => {},
  set: () => {},
  setColorTheme: () => {},
  setThemeVariant: () => {},
  setThemeEffects: () => {},
  setThemeSounds: () => {},
  prepareTheme: async () => {},
  forgetTheme: () => {},
});

function isValidTheme(v) { return v === 'system' || v === 'light' || v === 'dark'; }
function isValidEffects(v) { return v === true || v === false; }

export function ThemeProvider({ children }) {
  // Desktop 会在模块执行前注入稳定配置;普通 WebUI 则以系统/默认值起步。
  // 当前 origin 的 localStorage 仍作为首帧缓存,认证后由 App 用 daemon 配置校正。
  const initialAppearance = initialAppearancePreferences();
  const [themeMode, setTheme] = usePreference(
    STORAGE_KEY,
    initialAppearance.theme,
    isValidTheme,
  );
  const [colorTheme, setColorThemePreference] = usePreference(
    COLOR_THEME_STORAGE_KEY,
    initialAppearance.colorTheme,
    isValidColorTheme,
  );
  // Variant and motion follow the daemon like the other appearance values;
  // localStorage only paints the first frame before the daemon answers.
  const [themeVariant, setThemeVariantPreference] = usePreference(
    THEME_VARIANT_STORAGE_KEY,
    initialAppearance.themeVariant,
    validThemeVariantId,
  );
  const [themeEffects, setThemeEffectsPreference] = usePreference(
    THEME_EFFECTS_STORAGE_KEY,
    initialAppearance.themeEffects,
    isValidEffects,
  );
  const [themeSounds, setThemeSoundsPreference] = usePreference(
    THEME_SOUNDS_STORAGE_KEY,
    initialAppearance.themeSounds,
    isValidEffects,
  );
  const [systemTheme, setSystemTheme] = useState(() => effectiveAppearanceTheme('system'));
  const [installedThemes, setInstalledThemes] = useState({});
  const installedTheme = installedThemes[colorTheme] || null;
  const themeCache = useRef(new Map());
  const themeReloadRequired = useRef(new Set());
  const mounted = useRef(true);
  const forgetTheme = useCallback((id) => {
    releaseThemeResource(themeCache.current, id);
    themeReloadRequired.current.delete(id);
    setInstalledThemes((current) => {
      if (!current[id]) return current;
      const next = { ...current };
      delete next[id];
      return next;
    });
  }, []);
  const prepareTheme = useCallback(async (id, { refresh = false } = {}) => {
    if (!isInstalledColorTheme(id)) return null;
    refresh = refresh || themeReloadRequired.current.has(id);
    const previous = refresh ? themeCache.current.get(id) : null;
    if (refresh || !themeCache.current.has(id)) {
      const pending = (async () => {
        const definition = await api.getTheme(id);
        if (!validThemeDefinition(definition) || definition.id !== id) throw new Error('主题配色数据无效');
        return loadThemeResources(definition, (kind) => api.readThemeImage(id, kind, definition.version));
      })();
      themeCache.current.set(id, pending);
      // Keep the old image usable until its replacement is ready, then release
      // it even if the provider unmounts while either request is pending.
      if (previous) pending.then(() => previous.then((item) => revokeThemeResources(item))).catch(() => {
        // A deletion can evict a replacement while its image request fails.
        // The old image then has no cache owner left to release it.
        if (themeCache.current.get(id) !== pending && themeCache.current.get(id) !== previous) {
          void previous.then((item) => revokeThemeResources(item)).catch(() => {});
        }
      });
    }
    const pending = themeCache.current.get(id);
    try {
      const ready = await pending;
      if (mounted.current && themeCache.current.get(id) === pending) {
        themeReloadRequired.current.delete(id);
        setInstalledThemes((current) => ({ ...current, [id]: ready }));
      }
      return ready;
    } catch (error) {
      if (themeCache.current.get(id) === pending) {
        if (previous) {
          themeCache.current.set(id, previous);
          themeReloadRequired.current.add(id);
        }
        else themeCache.current.delete(id);
      }
      throw error;
    }
  }, []);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      const cache = themeCache.current;
      for (const pending of cache.values()) pending.then((item) => revokeThemeResources(item)).catch(() => {});
      cache.clear();
      themeReloadRequired.current.clear();
    };
  }, []);
  useEffect(() => {
    if (isInstalledColorTheme(colorTheme)) void prepareTheme(colorTheme).catch(() => {});
  }, [colorTheme, prepareTheme]);
  useEffect(() => {
    const media = globalThis.matchMedia?.('(prefers-color-scheme: dark)');
    if (!media) return undefined;
    const changed = () => setSystemTheme(media.matches ? 'dark' : 'light');
    changed();
    media.addEventListener?.('change', changed);
    return () => media.removeEventListener?.('change', changed);
  }, []);
  const activeTheme = installedTheme?.id === colorTheme ? installedTheme : null;
  const appearance = installedTheme ? resolveThemeAppearance(resolveThemeVariant(installedTheme, themeVariant)) : null;
  const themeChrome = activeTheme ? themeChromeStyle(activeTheme) : null;
  const soundStyle = activeTheme ? themeSoundStyle(activeTheme) : null;
  const variantOptions = useMemo(() => (activeTheme ? themeVariantOptions(activeTheme) : NO_VARIANTS), [activeTheme]);
  const activeThemeVariant = activeTheme ? selectThemeVariant(activeTheme, themeVariant)?.id || null : null;
  const theme = installedTheme?.mode || (colorTheme === EVA_THEME_ID ? 'light' : themeMode === 'system' ? systemTheme : themeMode);

  useLayoutEffect(() => {
    const root = document.documentElement;
    root.setAttribute('data-theme', theme);
    root.setAttribute('data-color-theme', colorTheme);
    const clearInstalledTheme = applyInstalledTheme(
      root,
      installedTheme?.id === colorTheme ? installedTheme : null,
      installedTheme?.backgroundUrl,
      installedTheme || {},
      { variant: themeVariant, effects: themeEffects },
    );
    // 两个主题维度落地后 --ace-bg 的 computed 值同步可读;把 body 底色推给
    // 桌面壳,native 换窗口打底色(快速 resize 的新暴露区域随主题,不闪黑/白)。
    // 非桌面壳环境内部 no-op。
    pushWindowBackgroundColor();
    // 只有声明了异形窗口的主题会让桌面壳切角;其它主题不发桥接(或恢复矩形)。
    pushWindowShape(installedTheme?.id === colorTheme ? themeWindowShape(installedTheme) : null);
    desktopTaskbarBadge.refresh();
    return clearInstalledTheme;
  }, [colorTheme, theme, installedTheme, themeVariant, themeEffects]);

  const toggle = useCallback(() => setTheme(theme === 'dark' ? 'light' : 'dark'), [setTheme, theme]);
  const set    = useCallback((t) => setTheme(isValidTheme(t) ? t : 'system'), [setTheme]);
  const setColorTheme = useCallback(
    (value) => setColorThemePreference(effectiveColorTheme(value)),
    [setColorThemePreference],
  );
  const setThemeVariant = useCallback(
    (value) => setThemeVariantPreference(validThemeVariantId(value) ? value : ''),
    [setThemeVariantPreference],
  );
  const setThemeEffects = useCallback(
    (value) => setThemeEffectsPreference(value !== false),
    [setThemeEffectsPreference],
  );
  const setThemeSounds = useCallback(
    (value) => setThemeSoundsPreference(value !== false),
    [setThemeSoundsPreference],
  );

  return (
    <ThemeCtx.Provider value={{
      theme, themeMode, colorTheme, appearance, toggle, set, setColorTheme, prepareTheme, forgetTheme,
      themeVariant, themeEffects, setThemeVariant, setThemeEffects, themeChrome,
      themeVariantOptions: variantOptions, activeThemeVariant,
      themeSounds, setThemeSounds, themeSoundStyle: soundStyle,
    }}>
      {children}
      <HudEffects
        chrome={themeChrome}
        effects={themeEffects}
        colorTheme={colorTheme}
        variant={activeThemeVariant}
        variantOptions={variantOptions}
      />
      <ThemeSoundLayer
        style={soundStyle}
        muted={!themeSounds}
        colorTheme={colorTheme}
        variant={activeThemeVariant}
      />
    </ThemeCtx.Provider>
  );
}

export function useTheme() { return useContext(ThemeCtx); }
