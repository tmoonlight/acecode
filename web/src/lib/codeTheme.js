// 代码块 / 文件预览的明暗配色(设置项 web_ui.code_theme)。
//
// - auto:跟随界面明暗(默认主题浅色界面 = 浅色代码,深色界面 = 深色代码);
// - light / dark:用户在代码块或文件预览右上角手动切换后的结果,经 daemon 外观偏好
//   全局保存;更换主题(明暗 / 配色主题 / 主题变体)时由 appearancePreferences.js 清回 auto。
// 已安装的主题包自带代码配色,不提供手动切换,只按它的代码底色判断明暗,
// 让语法高亮的 token 颜色与底色匹配。
//
// 结果写在 <html data-code-theme="light|dark">,样式见 globals.css「代码明暗」一节。

export const CODE_THEME_VALUES = Object.freeze(['auto', 'light', 'dark']);
export const DEFAULT_CODE_THEME = 'auto';
export const CODE_THEME_TOGGLE_SELECTOR = '[data-code-theme-toggle="true"]';

export function isValidCodeTheme(value) {
  return CODE_THEME_VALUES.includes(value);
}

export function normalizeCodeTheme(value) {
  return isValidCodeTheme(value) ? value : DEFAULT_CODE_THEME;
}

// #rrggbb 的感知亮度低于一半算深色;解析不了返回 null。
export function hexColorScheme(hex) {
  if (typeof hex !== 'string' || !/^#[0-9a-f]{6}$/i.test(hex)) return null;
  const [r, g, b] = [1, 3, 5].map((start) => parseInt(hex.slice(start, start + 2), 16));
  return (0.299 * r + 0.587 * g + 0.114 * b) / 255 < 0.5 ? 'dark' : 'light';
}

// 不手动切换时代码应有的明暗:主题包按它自己的代码底色,默认主题跟界面明暗。
export function naturalCodeTheme({ mode, installedCodeBg = null } = {}) {
  return hexColorScheme(installedCodeBg) || (mode === 'dark' ? 'dark' : 'light');
}

// 实际生效的明暗。主题包不接受手动值(按钮也不显示)。
export function effectiveCodeTheme({ preference, natural, installed = false } = {}) {
  if (!installed && (preference === 'light' || preference === 'dark')) return preference;
  return natural === 'dark' ? 'dark' : 'light';
}

// 点一下切到另一种明暗;切回与主题一致的那一种时存 auto,之后继续跟随界面明暗。
export function toggledCodeTheme({ effective, natural }) {
  const target = effective === 'dark' ? 'light' : 'dark';
  return target === natural ? 'auto' : target;
}

// 写根属性。instant=true 时先挂 data-code-theme-switching(globals.css 里把所有过渡
// 关掉),同步算完新配色再撤掉:过渡只在样式重算看到值变化时启动,这一次重算发生在
// 过渡被关的状态下,撤掉标记后值不再变化,也就不会补一段动画。
export function applyCodeThemeAttribute(root, value, { instant = false } = {}) {
  if (!root || root.getAttribute('data-code-theme') === value) return false;
  if (!instant) {
    root.setAttribute('data-code-theme', value);
    return true;
  }
  root.setAttribute('data-code-theme-switching', '');
  root.setAttribute('data-code-theme', value);
  void root.offsetWidth;
  root.removeAttribute('data-code-theme-switching');
  return true;
}

export function isCodeThemeToggleTarget(target) {
  return !!(target && typeof target.closest === 'function' && target.closest(CODE_THEME_TOGGLE_SELECTOR));
}
