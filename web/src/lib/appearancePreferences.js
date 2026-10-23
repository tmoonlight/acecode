import {
  DEFAULT_COLOR_THEME,
  isAiColorTheme,
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
  sidebarSessionTime: true,
  messageAutoCollapse: true,
  themeVariant: '',
  themeEffects: true,
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

// 布尔字段刻意宽松:只有显式 false 才关。旧 daemon 不返回这个键时按默认(开)
// 处理,不能让它把整份外观偏好判成「不支持」——那会连主题一起回退。
function inputSidebarSessionTime(value) {
  return (value?.sidebar_session_time ?? value?.sidebarSessionTime) !== false;
}

function inputMessageAutoCollapse(value) {
  return (value?.message_auto_collapse ?? value?.messageAutoCollapse) !== false;
}

// Variant IDs come from the selected theme package; an empty string means its
// first (default) variant. Unknown IDs are kept and resolved by the renderer.
export function validThemeVariantId(value) {
  return value === '' || (typeof value === 'string' && value.length <= 24
    && /^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(value));
}

function inputThemeVariant(value) {
  const variant = value?.theme_variant ?? value?.themeVariant;
  return validThemeVariantId(variant) ? variant : '';
}

// Theme motion defaults on; only an explicit false turns it off. Older daemons
// that do not return the key keep the default instead of failing the parse.
function inputThemeEffects(value) {
  return (value?.theme_effects ?? value?.themeEffects) !== false;
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
    sidebarSessionTime: inputSidebarSessionTime(value),
    messageAutoCollapse: inputMessageAutoCollapse(value),
    themeVariant: inputThemeVariant(value),
    themeEffects: inputThemeEffects(value),
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
    sidebarSessionTime: inputSidebarSessionTime(value),
    messageAutoCollapse: inputMessageAutoCollapse(value),
    themeVariant: inputThemeVariant(value),
    themeEffects: inputThemeEffects(value),
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
    sidebarSessionTime: patch?.sidebarSessionTime
      ?? patch?.sidebar_session_time
      ?? current?.sidebarSessionTime,
    messageAutoCollapse: patch?.messageAutoCollapse
      ?? patch?.message_auto_collapse
      ?? current?.messageAutoCollapse,
    themeVariant: patch?.themeVariant
      ?? patch?.theme_variant
      ?? current?.themeVariant,
    themeEffects: patch?.themeEffects
      ?? patch?.theme_effects
      ?? current?.themeEffects,
  }, scope);
}

export function appearancePreferencesToApi(value, scope = globalThis) {
  const normalized = normalizeAppearancePreferences(value, scope);
  return {
    show_acecode_avatar: false,
    theme: normalized.theme,
    color_theme: normalized.colorTheme,
    font_size: normalized.fontSize,
    sidebar_session_time: normalized.sidebarSessionTime,
    message_auto_collapse: normalized.messageAutoCollapse,
    theme_variant: normalized.themeVariant,
    theme_effects: normalized.themeEffects,
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
  const deletedColorThemes = new Set();
  const withoutDeletedTheme = (value) => deletedColorThemes.has(value.colorTheme)
    ? { ...value, colorTheme: DEFAULT_COLOR_THEME } : value;

  const applyIfActive = (value) => {
    if (active) apply?.(value);
  };

  return {
    restore(value) {
      const parsed = parseAppearancePreferences(value, scope);
      if (!parsed || revision !== 0 || deletedColorThemes.size > 0) return false;
      visible = parsed;
      confirmed = parsed;
      applyIfActive(parsed);
      return true;
    },

    change(patch, { silent = false, expectedAppearance } = {}) {
      const target = withoutDeletedTheme(mergeAppearancePreferences(visible, patch, scope));
      visible = target;
      revision += 1;
      const changeRevision = revision;
      applyIfActive(target);

      queue = queue
        .then(async () => {
          const payload = appearancePreferencesToApi(withoutDeletedTheme(target), scope);
          if (expectedAppearance) payload.expected_appearance = expectedAppearance;
          const response = await save(payload);
          const persisted = parseAppearancePreferences(response, scope);
          if (!persisted) {
            throw new Error('当前 daemon 不支持外观配置持久化');
          }
          confirmed = withoutDeletedTheme(persisted);
          if (changeRevision === revision) {
            visible = confirmed;
            applyIfActive(confirmed);
          }
        })
        .catch((error) => {
          if (changeRevision !== revision) return;
          visible = confirmed;
          applyIfActive(confirmed);
          if (active && !silent) onError(error);
        });
      return queue;
    },

    removeColorTheme(id, remove) {
      if (!isAiColorTheme(id)) return Promise.reject(new Error('内置主题不能导出或删除'));
      // Put removal between complete appearance snapshots. Later font/theme
      // changes may already be visible, so only replace references to this ID.
      const removal = queue.then(async () => {
        const result = await remove();
        if (result?.deleted !== true || result.id !== id) throw new Error('删除主题返回了无效结果，请刷新后重试');
        deletedColorThemes.add(id);
        confirmed = withoutDeletedTheme(parseAppearancePreferences(result.ui_preferences, scope) || confirmed);
        const next = withoutDeletedTheme(visible);
        if (next !== visible) {
          visible = next;
          applyIfActive(visible);
        }
        return result;
      });
      // The caller owns deletion feedback; a failed deletion cannot poison the
      // appearance queue or roll back unrelated, later user preferences.
      queue = removal.catch(() => {});
      return removal;
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
