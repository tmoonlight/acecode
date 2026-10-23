import { isAiColorTheme, isDownloadableColorTheme, isInstalledColorTheme, NATIONAL_DAY_THEME_ID } from './colorTheme.js';

export { NATIONAL_DAY_THEME_ID };
export const EVA_THEME_ID = 'eva-01';
export const BUILTIN_THEME_CARDS = Object.freeze([
  { id: NATIONAL_DAY_THEME_ID, name: '国庆节', thumbnail: '/themes/national-day-2026-thumbnail.png', swatches: ['#FFF8F2', '#FFFFFF', '#D9272E'] },
  { id: EVA_THEME_ID, name: 'EVA 初号机', thumbnail: '/themes/eva-01-thumbnail.png', swatches: ['#E9DEFA', '#F9F5FE', '#B7EF65'] },
]);
export const THEME_COLOR_KEYS = Object.freeze([
  'bg', 'surface', 'surface-alt', 'surface-hi', 'shell-hi', 'shell-bg', 'border',
  'border-soft', 'fg', 'fg-2', 'fg-mute', 'accent', 'accent-bg', 'accent-soft',
  'ok', 'ok-bg', 'ok-border', 'warn', 'warn-bg', 'danger', 'danger-bg',
  'code-bg', 'code-fg', 'code-line', 'selection', 'on-selection', 'send-bg', 'send-fg',
]);

export function validThemeHexColor(value) {
  return typeof value === 'string' && value.length === 7 && /^#[0-9a-f]{6}$/i.test(value);
}

export function validThemeAppearance(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  return Object.entries(value).every(([key, entry]) => {
    if (['logo_color', 'home_title_color', 'home_background_color', 'session_background_color', 'user_message_background_color'].includes(key)) return validThemeHexColor(entry);
    if (['home_composer_opacity', 'home_background_opacity', 'session_background_opacity', 'user_message_background_opacity'].includes(key)) {
      return typeof entry === 'number' && Number.isFinite(entry) && entry >= 0 && entry <= 1;
    }
    return key === 'extend_to_titlebar' && typeof entry === 'boolean';
  });
}

export function validThemeDefinition(value) {
  const identity = value?.id === EVA_THEME_ID ? value.mode === 'light'
    : (value?.id === NATIONAL_DAY_THEME_ID || isAiColorTheme(value?.id)) && typeof value.name === 'string' && !!value.name.trim()
      && ['light', 'dark'].includes(value.mode);
  return value?.schema_version === 1 && identity
    && typeof value.version === 'string' && !!value.version && value.colors && typeof value.colors === 'object'
    && Object.keys(value.colors).length === THEME_COLOR_KEYS.length
    && THEME_COLOR_KEYS.every((key) => validThemeHexColor(value.colors[key]))
    && (!Object.hasOwn(value, 'appearance') || validThemeAppearance(value.appearance))
    && ['session_background', 'user_message_background'].every((key) => !Object.hasOwn(value, key)
      || (Number.isSafeInteger(value[key]?.bytes) && value[key].bytes > 0 && value[key].bytes <= 16 * 1024 * 1024
        && typeof value[key].sha256 === 'string' && /^[a-f0-9]{64}$/i.test(value[key].sha256)));
}

// Optional chrome/variants live at the top level so older clients and the
// published workshop validator keep accepting the package and simply render
// the default palette. Unknown or malformed values are ignored, never applied.
export const THEME_CHROME_STYLES = Object.freeze(['hud']);
export const MAX_THEME_VARIANTS = 6;
const VARIANT_KEYS = new Set(['id', 'name', 'colors', 'appearance', 'tint']);

export function themeChromeStyle(definition) {
  const chrome = definition?.chrome;
  if (!chrome || typeof chrome !== 'object' || Array.isArray(chrome)) return null;
  return THEME_CHROME_STYLES.includes(chrome.style) ? chrome.style : null;
}

export function validThemeVariant(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return false;
  if (!Object.keys(value).every((key) => VARIANT_KEYS.has(key))) return false;
  const name = typeof value.name === 'string' ? value.name.trim() : '';
  return typeof value.id === 'string' && value.id.length <= 24 && /^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(value.id)
    && !!name && [...name].length <= 24 && !/[\u0000-\u001f\u007f]/.test(value.name)
    && !!value.colors && typeof value.colors === 'object' && !Array.isArray(value.colors)
    && Object.keys(value.colors).length === THEME_COLOR_KEYS.length
    && THEME_COLOR_KEYS.every((key) => validThemeHexColor(value.colors[key]))
    && (!Object.hasOwn(value, 'appearance') || validThemeAppearance(value.appearance))
    && (!Object.hasOwn(value, 'tint') || validThemeHexColor(value.tint));
}

// All-or-nothing: a partly broken list would change which choice is default.
export function themeVariants(definition) {
  const list = definition?.variants;
  if (!Array.isArray(list) || list.length < 2 || list.length > MAX_THEME_VARIANTS) return [];
  const ids = new Set();
  for (const variant of list) {
    if (!validThemeVariant(variant) || ids.has(variant.id)) return [];
    ids.add(variant.id);
  }
  return list;
}

// The first variant is the default; a stale or foreign ID falls back to it.
export function selectThemeVariant(definition, variantId) {
  const variants = themeVariants(definition);
  if (!variants.length) return null;
  return variants.find((variant) => variant.id === variantId) || variants[0];
}

// Returns the definition that rendering uses: the selected variant replaces
// the palette and overlays its appearance keys; everything else is unchanged.
export function resolveThemeVariant(definition, variantId) {
  const variant = selectThemeVariant(definition, variantId);
  if (!variant) return definition;
  const appearance = { ...(definition.appearance || {}), ...(variant.appearance || {}) };
  return {
    ...definition,
    colors: variant.colors,
    ...(Object.keys(appearance).length ? { appearance } : {}),
  };
}

export function themeVariantOptions(definition) {
  return themeVariants(definition).map((variant) => ({
    id: variant.id,
    name: variant.name.trim(),
    swatches: [variant.colors.bg, variant.colors.accent, variant.colors['send-bg']],
  }));
}

export function resolveThemeAppearance(definition) {
  if (!validThemeDefinition(definition)) throw new Error('主题配色数据无效');
  const appearance = definition.appearance || {};
  const legacyEvaTitlebar = definition.id === EVA_THEME_ID && !Object.hasOwn(appearance, 'extend_to_titlebar');
  const extendToTitlebar = appearance.extend_to_titlebar ?? definition.id === EVA_THEME_ID;
  return {
    logoColor: appearance.logo_color ?? null,
    homeTitleColor: appearance.home_title_color ?? definition.colors.fg,
    extendToTitlebar,
    whiteTitlebarControls: extendToTitlebar && (definition.mode === 'dark' || legacyEvaTitlebar),
  };
}

const BACKGROUND_RESOURCES = Object.freeze([
  { key: 'background', kind: 'background', url: 'backgroundUrl', region: 'home', color: 'bg' },
  { key: 'session_background', kind: 'session-background', url: 'sessionBackgroundUrl', region: 'session', color: 'bg' },
  { key: 'user_message_background', kind: 'user-message-background', url: 'userMessageBackgroundUrl', region: 'user-message', color: 'accent-bg' },
]);
const rgb = (hex) => [1, 3, 5].map((start) => parseInt(hex.slice(start, start + 2), 16)).join(', ');

export function revokeThemeResources(item, revoke = (url) => URL.revokeObjectURL(url)) {
  const urls = new Set(BACKGROUND_RESOURCES.map((resource) => item?.[resource.url]).filter(Boolean));
  for (const url of urls) revoke(url);
}

export async function loadThemeResources(definition, readImage, createUrl = (blob) => URL.createObjectURL(blob), revoke = (url) => URL.revokeObjectURL(url)) {
  if (!validThemeDefinition(definition)) throw new Error('主题配色数据无效');
  const item = { ...definition, backgroundUrl: null, sessionBackgroundUrl: null, userMessageBackgroundUrl: null };
  const resources = BACKGROUND_RESOURCES.filter((resource) => resource.key === 'background' || Object.hasOwn(definition, resource.key));
  // Wait for every request before cleanup: a late successful image must not leak
  // its URL after another image has already failed.
  const results = await Promise.allSettled(resources.map(async (resource) => {
    const blob = await readImage(resource.kind);
    item[resource.url] = createUrl(blob);
  }));
  const failure = results.find((result) => result.status === 'rejected');
  if (failure) { revokeThemeResources(item, revoke); throw failure.reason; }
  return item;
}

export function themeCssProperties(definition, backgroundUrl, extraUrls = {}, { tint = null } = {}) {
  if (!validThemeDefinition(definition)) throw new Error('主题配色数据无效');
  // A variant tint keeps each artwork's luminance and takes the tint's hue
  // (CSS "color" blending), so one set of images serves every variant.
  const tintLayer = validThemeHexColor(tint) ? `linear-gradient(${tint}, ${tint}), ` : '';
  const result = {};
  for (const key of THEME_COLOR_KEYS) {
    const hex = definition.colors[key];
    result[`--ace-${key}`] = hex;
    result[`--ace-${key}-rgb`] = [1, 3, 5].map((start) => parseInt(hex.slice(start, start + 2), 16)).join(', ');
  }
  const appearance = resolveThemeAppearance(definition);
  result['--ace-home-title-color'] = appearance.homeTitleColor;
  if (appearance.logoColor) {
    result['--ace-logo-color'] = appearance.logoColor;
    result['--ace-logo-color-rgb'] = [1, 3, 5].map((start) => parseInt(appearance.logoColor.slice(start, start + 2), 16)).join(', ');
  }
  const overrides = definition.appearance || {};
  if (overrides.home_composer_opacity !== undefined) result['--ace-home-composer-opacity'] = String(overrides.home_composer_opacity);
  // Only application-created blob URLs may become CSS image values. Opacity
  // blends artwork into its backdrop without fading any text or controls.
  for (const resource of BACKGROUND_RESOURCES) {
    if (resource.key !== 'background' && !Object.hasOwn(definition, resource.key)) continue;
    const url = resource.key === 'background' ? backgroundUrl : extraUrls[resource.url];
    const key = resource.region.replaceAll('-', '_') + '_background';
    const color = overrides[`${key}_color`] ?? definition.colors[resource.color];
    if (overrides[`${key}_color`] !== undefined) result[`--ace-${resource.region}-background-color`] = color;
    if (typeof url !== 'string' || !url.startsWith('blob:') || /["()\s]/.test(url)) continue;
    const opacity = overrides[`${key}_opacity`];
    const veil = opacity === undefined ? '' : `linear-gradient(rgba(${rgb(color)}, ${1 - opacity}), rgba(${rgb(color)}, ${1 - opacity})), `;
    result[`--ace-${resource.region}-background-image`] = `${veil}${tintLayer}url("${url}")`;
    if (tintLayer) result[`--ace-${resource.region}-background-blend`] = veil ? 'normal, color, normal' : 'color, normal';
    if (resource.region === 'user-message') {
      // A shared artwork width keeps decorations identical in every message.
      // Only the color veil (and tint) stretch with the bubble; artwork is cropped.
      const fill = (veil ? '100% 100%, ' : '') + (tintLayer ? '100% 100%, ' : '');
      result['--ace-user-message-background-size'] = `${fill}720px auto`;
    }
  }
  return result;
}

export function applyInstalledTheme(root, definition, backgroundUrl, extraUrls = {}, { variant = '', effects = true } = {}) {
  const selected = definition ? selectThemeVariant(definition, variant) : null;
  const effective = definition ? resolveThemeVariant(definition, variant) : null;
  const chrome = definition ? themeChromeStyle(definition) : null;
  const properties = effective ? themeCssProperties(effective, backgroundUrl, extraUrls, { tint: selected?.tint }) : {};
  const appearance = effective ? resolveThemeAppearance(effective) : null;
  const hasBackground = !!properties['--ace-home-background-image'];
  const attributes = {
    'data-installed-theme': definition?.id,
    'data-theme-wallpaper': hasBackground ? 'true' : null,
    'data-theme-session-background': properties['--ace-session-background-image'] ? 'true' : null,
    'data-theme-user-message-background': properties['--ace-user-message-background-image'] ? 'true' : null,
    'data-theme-extend-to-titlebar': hasBackground ? String(appearance.extendToTitlebar) : null,
    'data-theme-titlebar-controls': hasBackground && appearance.whiteTitlebarControls ? 'white' : null,
    'data-theme-logo-color': appearance?.logoColor ? 'custom' : null,
    'data-theme-chrome': chrome,
    'data-theme-variant': selected?.id || null,
    'data-theme-effects': chrome ? (effects === false ? 'off' : 'on') : null,
  };
  for (const [key, value] of Object.entries(properties)) root.style.setProperty(key, value);
  for (const [key, value] of Object.entries(attributes)) {
    if (value) root.setAttribute(key, value);
    else root.removeAttribute(key);
  }
  return () => {
    for (const key of Object.keys(properties)) root.style.removeProperty(key);
    for (const key of Object.keys(attributes)) root.removeAttribute(key);
  };
}

export function themeDownloadConsent(entry) {
  if (!isDownloadableColorTheme(entry?.id) || !Number.isSafeInteger(entry?.package?.bytes)
      || entry.package.bytes <= 0 || !/^[a-f0-9]{64}$/i.test(entry.package.sha256)
      || typeof entry.version !== 'string') throw new Error('暂时无法获取主题下载大小，请重试');
  return { confirm_download: true, version: entry.version, bytes: entry.package.bytes, sha256: entry.package.sha256 };
}

export function themeDownloadPercent(job) {
  if (!(job?.bytes_total > 0)) return 0;
  return Math.max(0, Math.min(100, Math.floor((job.bytes_downloaded || 0) / job.bytes_total * 100)));
}

export function themePackageSize(bytes) {
  return `${(Math.max(0, Number(bytes) || 0) / 1000000).toFixed(2)} MB`;
}

export function themeJobActive(job) { return ['downloading', 'installing'].includes(job?.state); }

export function themeErrorText(error) {
  const code = typeof error === 'string' ? error : error?.code;
  if (code === 'THEME_CONFIRMATION_REQUIRED') return '主题包已更新，请重新确认下载大小';
  if (code === 'THEME_INVALID_PACKAGE') return '主题包校验失败，请重新下载';
  if (code === 'THEME_NOT_INSTALLED') return '主题资源尚未下载或已损坏，请重新下载';
  if (code === 'THEME_CATALOG_UNAVAILABLE') return '暂时无法获取主题信息，请检查网络后重试';
  if (code === 'THEME_DOWNLOAD_BUSY') return '主题正在下载，请稍候';
  if (code === 'THEME_SAVE_FAILED') return '无法保存主题资源，请检查文件写入权限';
  return '主题下载或应用失败，请重试';
}

export function themeFailure(error, fallbackPath = '') {
  const path = error?.body?.error_path || error?.error_path || fallbackPath;
  const detail = error?.body?.message || (!error?.code ? error?.message : '');
  const message = typeof detail === 'string' && detail ? detail : themeErrorText(error?.error || error);
  return { message, path: typeof path === 'string' ? path : '' };
}

export function localThemeEntries(catalog) {
  const seen = new Set();
  return (Array.isArray(catalog?.themes) ? catalog.themes : []).filter((entry) => {
    if (!isAiColorTheme(entry?.id) || entry.source !== 'local' || entry.installed !== true
        || typeof entry.name !== 'string' || !entry.name.trim() || seen.has(entry.id)) return false;
    seen.add(entry.id);
    return true;
  });
}

export function releaseThemeResource(cache, id, revoke = (url) => URL.revokeObjectURL(url)) {
  const pending = cache.get(id);
  cache.delete(id);
  if (pending) Promise.resolve(pending).then((item) => revokeThemeResources(item, revoke)).catch(() => {});
}

// The controller outlives Settings. A later selection revokes an older
// download's automatic application, while the useful local install can finish.
export function createThemeDownloadController({ api, prepare, apply, remove = (id) => api.deleteTheme(id), forget, onChange, wait = () => new Promise((resolve) => setTimeout(resolve, 400)) }) {
  let active = true, revision = 0, polling = null, intent = null;
  let startup = null, userIntent = false;
  const removedIds = new Set();
  let state = { entry: null, entries: [], localEntries: [], deletingId: '', loading: false, error: '', failure: null, job: { state: 'idle' } };
  const patch = (value) => { state = { ...state, ...value }; if (active) onChange?.(state); };
  const report = (error, notify = false, path = state.entry?.package?.url || state.entry?.package?.path || '', silent = false) => {
    if (silent) return;
    const failure = themeFailure(error, path);
    patch({ error: failure.message, ...(notify ? { failure } : {}) });
  };
  const consumeJob = async (job, notify = false) => {
    job = { automatic: state.job.automatic === true && state.job.id === job.id, ...job };
    patch({ job });
    if (job.state === 'completed') {
      // A passive refresh can replay completion of an older version. It must
      // not clear an update offered by a newer catalogue.
      const updated = (entry) => entry?.id === job.id ? {
        ...entry,
        installed: true,
        installed_version: job.version || entry.installed_version,
        update_available: job.version === entry.version ? false : entry.update_available,
      } : entry;
      patch({ entry: updated(state.entry), entries: state.entries.map(updated), error: '' });
      const pending = intent;
      if (pending && pending.revision === revision && pending.id === job.id && pending.version === job.version) {
        intent = null;
        await prepare(job.id, { refresh: true });
        if (active && pending.revision === revision) await apply(job.id, {
          silent: pending.automatic === true, expectedAppearance: pending.expectedAppearance,
        });
      }
    } else if (job.state === 'failed') { intent = null; report(job, notify, undefined, job.automatic); }
    else if (job.state === 'cancelled') intent = null;
  };
  const poll = () => {
    if (polling) return polling;
    polling = (async () => {
      while (active && themeJobActive(state.job)) {
        await wait();
        if (!active) return;
        await consumeJob(await api.getThemeJob(), true);
      }
    })().catch((error) => {
      if (!active) return;
      intent = null;
      if (state.job.automatic) patch({ job: { ...state.job, state: 'failed' } });
      report(error, true, undefined, state.job.automatic);
    })
      .finally(() => { polling = null; });
    return polling;
  };
  return {
    state: () => state,
    dismissFailure() { patch({ failure: null }); },
    activate() { active = true; },
    dispose() { active = false; revision += 1; intent = null; },
    async refresh({ notify = false, id = EVA_THEME_ID, silent = false } = {}) {
      const notifyFailure = notify || themeJobActive(state.job);
      patch({ loading: true });
      try {
        const catalog = await api.getThemes(true);
        const entries = (catalog?.themes || []).filter((item) => isDownloadableColorTheme(item.id));
        const entry = entries.find((item) => item.id === EVA_THEME_ID) || null;
        // Local themes remain available when the remote catalogue is offline.
        // An installed EVA descriptor can also lack download metadata offline.
        for (const item of entries) if (!item.installed && item.available !== false) themeDownloadConsent(item);
        patch({ entry, entries, localEntries: localThemeEntries(catalog).filter((item) => !removedIds.has(item.id)), loading: false,
          error: catalog.catalog_error && !silent && (notify || !state.job.automatic) ? themeErrorText(catalog.catalog_error.error) : '' });
        await consumeJob(catalog.job || { state: 'idle' }, themeJobActive(state.job));
        if (themeJobActive(state.job)) void poll();
        return entries.find((item) => item.id === id) || null;
      } catch (error) { patch({ loading: false }); report(error, notifyFailure, undefined, silent || (!notify && state.job.automatic)); return null; }
    },
    applyStartupTheme(expectedAppearance) {
      if (startup) return startup;
      const selected = revision;
      const eligible = () => active && !userIntent && selected === revision && !themeJobActive(state.job);
      startup = (async () => {
        const claim = await api.claimStartupTheme();
        if (claim?.claimed !== true || claim.id !== NATIONAL_DAY_THEME_ID || !eligible()) return;
        const entry = await this.refresh({ id: NATIONAL_DAY_THEME_ID, silent: true });
        if (!entry || !eligible()) return;
        if (entry.installed) {
          await prepare(entry.id);
          if (eligible()) await apply(entry.id, { silent: true, expectedAppearance });
        } else if (entry.available !== false) {
          await this.install(entry, { automatic: true, expectedAppearance });
        }
      })().catch(() => {});
      return startup;
    },
    async select(id) {
      userIntent = true;
      if (state.deletingId === id || removedIds.has(id)) return;
      const selected = ++revision;
      intent = null;
      try {
        if (isInstalledColorTheme(id)) await prepare(id);
        if (active && selected === revision && state.deletingId !== id && !removedIds.has(id)) await apply(id);
      } catch (error) { report(error, true); }
    },
    beginCreation() {
      userIntent = true;
      intent = null;
      return ++revision;
    },
    async importLocal(file, digest, applyAfter = true) {
      userIntent = true;
      if (state.deletingId) throw new Error('主题正在处理中，请稍后重试');
      const selected = ++revision;
      intent = null;
      const result = await api.importTheme(file, digest);
      if (!isAiColorTheme(result?.id)) throw new Error('导入主题返回了无效结果，请刷新后重试');
      removedIds.delete(result.id);
      forget?.(result.id);
      await this.refresh();
      let applied = false;
      if (applyAfter && active && selected === revision) {
        await prepare(result.id, { refresh: true });
        if (active && selected === revision) { await apply(result.id); applied = true; }
      }
      return { ...result, applied };
    },
    async created(theme, selected) {
      if (!isAiColorTheme(theme?.id) || theme.apply !== true || removedIds.has(theme.id)) return;
      try {
        await this.refresh();
        if (!active || !Number.isSafeInteger(selected) || selected !== revision) return;
        await prepare(theme.id, { refresh: true });
        if (active && selected === revision && state.deletingId !== theme.id && !removedIds.has(theme.id)) await apply(theme.id);
      } catch (error) { report(error, true, theme.id); }
    },
    async remove(id) {
      if (!isAiColorTheme(id)) throw new Error('内置主题不能导出或删除');
      if (state.deletingId) throw new Error('主题正在处理中，请稍后重试');
      patch({ deletingId: id });
      try {
        const result = await remove(id);
        if (result?.deleted !== true || result.id !== id) throw new Error('删除主题返回了无效结果，请刷新后重试');
        removedIds.add(id);
        if (intent?.id === id) intent = null;
        forget?.(id);
        patch({ localEntries: state.localEntries.filter((item) => item.id !== id) });
        // Removing a local theme succeeds offline; never await the remote
        // catalogue before removing its card and releasing its image.
        void this.refresh();
        return result;
      } finally { patch({ deletingId: '' }); }
    },
    async install(entry, { automatic = false, expectedAppearance } = {}) {
      if (!automatic) userIntent = true;
      const selected = ++revision;
      intent = { id: entry.id, version: entry.version, revision: selected, automatic, expectedAppearance };
      try {
        patch({ error: '', failure: null, job: { id: entry.id, state: 'downloading', bytes_downloaded: 0, bytes_total: entry.package.bytes, automatic } });
        const consent = themeDownloadConsent(entry);
        const job = await api.installTheme(entry.id, automatic ? { ...consent, automatic: true } : consent);
        // Even a very fast install is observed by polling once so the same
        // completion path applies the theme and respects the selection revision.
        patch({ job: { automatic, ...job, state: themeJobActive(job) ? job.state : 'installing' } });
        void poll();
      } catch (error) {
        intent = null;
        patch({ job: { id: entry.id, state: 'failed', automatic } });
        report(error, true, entry.package?.url || entry.package?.path, automatic);
      }
    },
    async cancel() {
      revision += 1; intent = null;
      try { await api.cancelThemeInstall(); if (themeJobActive(state.job)) void poll(); }
      catch (error) { report(error, true); }
    },
  };
}
