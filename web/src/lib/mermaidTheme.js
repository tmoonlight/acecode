import { colorContrast } from './colorContrast.js';
import { normalizeThemeBackgroundColor } from './desktopWindowBackground.js';

function backgroundChannels(value) {
  const hex = normalizeThemeBackgroundColor(value);
  if (hex) return [...[1, 3, 5].map((offset) => parseInt(hex.slice(offset, offset + 2), 16)), 1];
  const match = String(value || '').trim().match(/^rgba?\(([^)]+)\)$/i);
  if (!match) return null;
  const parts = match[1].trim().split(/[\s,/]+/);
  if (parts.length < 3 || parts.length > 4 || parts.some((part) => !/^\d*\.?\d+%?$/.test(part))) return null;
  return [0, 1, 2, 3].map((index) => {
    if (index === 3 && parts[index] === undefined) return 1;
    const limit = index === 3 ? 1 : 255;
    const channel = Number.parseFloat(parts[index]) * (parts[index].endsWith('%') ? limit / 100 : 1);
    return Math.min(limit, channel);
  });
}

export function mermaidTheme(doc = globalThis.document, frame = doc?.body || doc?.documentElement) {
  const fallback = doc?.documentElement?.getAttribute?.('data-theme') === 'dark' ? 'dark' : 'light';
  const win = doc?.defaultView;
  if (typeof win?.getComputedStyle !== 'function') return fallback;

  // Composite transparent ancestors from front to back; the theme flag alone can
  // disagree with a custom theme's actual chat background.
  const channels = [0, 0, 0];
  let remaining = 1;
  try {
    for (let element = frame; element; element = element.parentElement) {
      const color = backgroundChannels(win.getComputedStyle(element).backgroundColor);
      if (!color) continue;
      const alpha = color[3] * remaining;
      for (let index = 0; index < 3; index += 1) channels[index] += color[index] * alpha;
      remaining *= 1 - color[3];
      if (remaining === 0) break;
    }
  } catch {
    return fallback;
  }
  const canvas = fallback === 'dark' ? 51 : 255;
  const background = '#' + channels.map((channel) => Math.round(channel + canvas * remaining)
    .toString(16).padStart(2, '0')).join('');
  return colorContrast(background, '#ffffff') > colorContrast(background, '#000000') ? 'dark' : 'light';
}

function mermaidInk(theme) {
  return theme === 'dark' ? '#ffffff' : '#000000';
}

function hexColor(channels) {
  return '#' + channels.slice(0, 3).map((channel) => Math.round(channel).toString(16).padStart(2, '0')).join('');
}

// 浏览器里借 canvas 把命名色、hsl() 等写法规范成 #rrggbb / rgba();Node 测试没有
// document,只认 hex 与 rgb()/rgba()。两次用不同底色赋值,结果不同说明这个值无效。
let colorProbe = null;
function resolveCssColor(value) {
  const doc = globalThis.document;
  if (typeof doc?.createElement !== 'function') return null;
  try {
    if (!colorProbe) colorProbe = doc.createElement('canvas').getContext('2d');
    if (!colorProbe) return null;
    colorProbe.fillStyle = '#000000';
    colorProbe.fillStyle = value;
    const first = colorProbe.fillStyle;
    colorProbe.fillStyle = '#ffffff';
    colorProbe.fillStyle = value;
    return first === colorProbe.fillStyle ? first : null;
  } catch {
    return null;
  }
}

// 纠正后背景块最亮的通道压到这里(深色)/ 最暗的通道抬到这里(浅色):通道之间的
// 差值不变,色调就还在;深色下任何最大通道 ≤ 64 的颜色与白字对比度都 ≥ 10,
// 浅色下最小通道 ≥ 208 的颜色与黑字对比度都 ≥ 13,不需要再迭代。
const DARK_BLOCK_PEAK = 64;
const LIGHT_BLOCK_FLOOR = 208;
// 与 mermaidTheme 的兜底画布一致:半透明背景块按这个底色合成后再判对比度。
const DARK_CANVAS = 51;
const LIGHT_CANVAS = 255;

function readableBlockFill(value, theme, resolveColor) {
  const channels = backgroundChannels(value) || backgroundChannels(resolveColor(value));
  if (!channels || channels[3] === 0) return null;
  const [red, green, blue, alpha] = channels;
  const canvas = theme === 'dark' ? DARK_CANVAS : LIGHT_CANVAS;
  const painted = [red, green, blue].map((channel) => channel * alpha + canvas * (1 - alpha));
  if (colorContrast(hexColor(painted), mermaidInk(theme)) >= 4.5) return null;

  const shifted = theme === 'dark'
    ? [red, green, blue].map((channel) => Math.max(0, channel - (Math.max(red, green, blue) - DARK_BLOCK_PEAK)))
    : [red, green, blue].map((channel) => Math.min(255, channel + (LIGHT_BLOCK_FLOOR - Math.min(red, green, blue))));
  const [r, g, b] = shifted.map((channel) => Math.round(channel));
  return alpha === 1 ? `rgb(${r}, ${g}, ${b})` : `rgba(${r}, ${g}, ${b}, ${Math.round(alpha * 1000) / 1000})`;
}

// 序列图的 `rect <颜色>` 高亮块和 `box <颜色>` 参与者分组由 mermaid 画成
// <rect class="rect" fill="源码里的颜色">,主题变量管不到;而文字墨色按主题统一成
// 纯白 / 纯黑,模型照官方示例写的浅色高亮块在深色主题下就成了白底白字。这里只改
// 与墨色对比度不够的背景块:保持色调,把亮度移到墨色的另一侧。原地修改 SVG 根节点。
export function adaptMermaidBackgroundFills(root, theme, { resolveColor = resolveCssColor } = {}) {
  if (root?.getAttribute?.('aria-roledescription') !== 'sequence') return;
  for (const rect of Array.from(root.getElementsByTagName('rect'))) {
    if (!String(rect.getAttribute('class') || '').split(/\s+/).includes('rect')) continue;
    const fill = readableBlockFill(String(rect.getAttribute('fill') || ''), theme, resolveColor);
    if (fill) rect.setAttribute('fill', fill);
  }
}

export function mermaidThemeVariables(theme) {
  const ink = mermaidInk(theme);
  // Keep Mermaid's node fills, but give every supported family's default ink
  // enough contrast on the transparent canvas and on those matching fills.
  return {
    textColor: ink,
    primaryTextColor: ink,
    secondaryTextColor: ink,
    tertiaryTextColor: ink,
    lineColor: ink,
    defaultLinkColor: ink,
    arrowheadColor: ink,
    nodeBorder: ink,
    clusterBorder: ink,
    actorBorder: ink,
    actorLineColor: ink,
    actorTextColor: ink,
    signalColor: ink,
    signalTextColor: ink,
    labelBoxBorderColor: ink,
    labelTextColor: ink,
    loopTextColor: ink,
    noteBorderColor: ink,
    noteTextColor: ink,
    activationBorderColor: ink,
    transitionColor: ink,
    transitionLabelColor: ink,
    stateLabelColor: ink,
    compositeBorder: ink,
    specialStateColor: ink,
    classText: ink,
  };
}
