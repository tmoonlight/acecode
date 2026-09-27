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

export function mermaidThemeVariables(theme) {
  const ink = theme === 'dark' ? '#ffffff' : '#000000';
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
