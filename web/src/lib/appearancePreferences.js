import {
  DEFAULT_COLOR_THEME,
  isValidColorTheme,
} from './colorTheme.js';
import {
  DEFAULT_FONT_SIZE,
  FONT_SIZE_VALUES,
} from './uiPrefs.js';

export const APPEARANCE_THEME_VALUES = Object.freeze(['system', 'light', 'dark']);
export const DEFAULT_APPEARANCE_PREFERENCES = Object.freeze({
  theme: 'system',
  colorTheme: DEFAULT_COLOR_THEME,
  fontSize: DEFAULT_FONT_SIZE,
});

const FONT_SIZE_SET = new Set(FONT_SIZE_VALUES);

export function systemThemeFallback(scope = globalThis) {
  const matchMedia = scope?.matchMedia || scope?.window?.matchMedia;
  try {
    return typeof matchMedia === 'function'
      && matchMedia.call(scope?.window || scope, '(prefers-color-scheme: dark)').matches
      ? 'dark'
      : 'light';
  } catch {
    return 'light';
  }
}

function inputColorTheme(value) {
  return value?.color_theme ?? value?.colorTheme;
}

function inputFontSize(value) {
  return value?.font_size ?? value?.fontSize;
}

export function effectiveAppearanceTheme(value, scope = globalThis) {
  return value === 'light' || value === 'dark'
    ? value
    : systemThemeFallback(scope);
}

export function normalizeAppearancePreferences(value, scope = globalThis) {
  const themePreference = APPEARANCE_THEME_VALUES.includes(value?.theme)
    ? value.theme
    : DEFAULT_APPEARANCE_PREFERENCES.theme;
  const colorTheme = inputColorTheme(value);
  const fontSize = inputFontSize(value);
  return {
    theme: themePreference,
    colorTheme: isValidColorTheme(colorTheme)
      ? colorTheme
      : DEFAULT_APPEARANCE_PREFERENCES.colorTheme,
    fontSize: FONT_SIZE_SET.has(fontSize)
      ? fontSize
      : DEFAULT_APPEARANCE_PREFERENCES.fontSize,
  };
}

export function parseAppearancePreferences(value, scope = globalThis) {
  if (!value || typeof value !== 'object') return null;
  const colorTheme = inputColorTheme(value);
  const fontSize = inputFontSize(value);
  if (!APPEARANCE_THEME_VALUES.includes(value.theme)
      || !isValidColorTheme(colorTheme)
      || !FONT_SIZE_SET.has(fontSize)) {
    return null;
  }
  return {
    theme: value.theme,
    colorTheme,
    fontSize,
  };
}

export function appearanceBootstrapPreferences(scope = globalThis) {
  return parseAppearancePreferences(scope?.__ACECODE_APPEARANCE__, scope);
}

export function initialAppearancePreferences(scope = globalThis) {
  return appearanceBootstrapPreferences(scope)
    || normalizeAppearancePreferences(DEFAULT_APPEARANCE_PREFERENCES, scope);
}

export function mergeAppearancePreferences(current, patch, scope = globalThis) {
  return normalizeAppearancePreferences({
    theme: patch?.theme ?? current?.theme,
    colorTheme: patch?.colorTheme ?? patch?.color_theme ?? current?.colorTheme,
    fontSize: patch?.fontSize ?? patch?.font_size ?? current?.fontSize,
  }, scope);
}

export function appearancePreferencesToApi(value, scope = globalThis) {
  const normalized = normalizeAppearancePreferences(value, scope);
  return {
    show_acecode_avatar: false,
    theme: normalized.theme,
    color_theme: normalized.colorTheme,
    font_size: normalized.fontSize,
  };
}

export function createAppearancePersistenceController({
  initial,
  apply,
  save,
  onError = () => {},
  scope = globalThis,
}) {
  let visible = normalizeAppearancePreferences(initial, scope);
  let confirmed = visible;
  let revision = 0;
  let queue = Promise.resolve();
  let active = true;

  const applyIfActive = (value) => {
    if (active) apply?.(value);
  };

  return {
    restore(value) {
      const parsed = parseAppearancePreferences(value, scope);
      if (!parsed || revision !== 0) return false;
      visible = parsed;
      confirmed = parsed;
      applyIfActive(parsed);
      return true;
    },

    change(patch) {
      const target = mergeAppearancePreferences(visible, patch, scope);
      visible = target;
      revision += 1;
      const changeRevision = revision;
      applyIfActive(target);

      queue = queue
        .then(async () => {
          const response = await save(appearancePreferencesToApi(target, scope));
          const persisted = parseAppearancePreferences(response, scope);
          if (!persisted) {
            throw new Error('当前 daemon 不支持外观配置持久化');
          }
          confirmed = persisted;
          if (changeRevision === revision) {
            visible = persisted;
            applyIfActive(persisted);
          }
        })
        .catch((error) => {
          if (changeRevision !== revision) return;
          visible = confirmed;
          applyIfActive(confirmed);
          if (active) onError(error);
        });
      return queue;
    },

    current() {
      return visible;
    },

    confirmed() {
      return confirmed;
    },

    idle() {
      return queue;
    },

    dispose() {
      active = false;
    },
  };
}
