// Resolve one actually monospaced face before xterm measures its character grid.
// On WebKitGTK a missing first family can resolve to proportional text and mask
// every later family in a CSS stack, including the generic monospace fallback.
export const CONSOLE_FONT_SIZE = 13;

const FALLBACK_FONT = 'monospace';
const FONT_CANDIDATES = [
  '"SF Mono"', '"Cascadia Code"', '"Fira Code"', 'Menlo', 'Consolas',
  '"DejaVu Sans Mono"', '"Liberation Mono"', '"Ubuntu Mono"',
];
const PROBE_CHARACTERS = 'WiMl09 .,:;@#_-/\\[]()';
const WIDTH_EPSILON = 0.05;

function hasMonospaceMetrics(context, family, fontSize) {
  let cellWidth;
  for (const weight of ['normal', 'bold']) {
    context.font = `${weight} ${fontSize}px ${family}`;
    for (const character of PROBE_CHARACTERS) {
      const width = context.measureText(character).width;
      if (!Number.isFinite(width) || width <= 0) return false;
      if (cellWidth === undefined) cellWidth = width;
      // Validate across weights too: a bold prompt must use the same grid.
      if (Math.abs(width - cellWidth) > WIDTH_EPSILON) return false;
    }
  }
  return true;
}

export function resolveConsoleFont(document, fontSize = CONSOLE_FONT_SIZE) {
  if (!Number.isFinite(fontSize) || fontSize <= 0) return FALLBACK_FONT;

  let context;
  try {
    context = document?.createElement('canvas').getContext('2d');
  } catch {
    return FALLBACK_FONT;
  }
  if (!context) return FALLBACK_FONT;

  for (const family of FONT_CANDIDATES) {
    try {
      if (hasMonospaceMetrics(context, family, fontSize)) return family;
    } catch {
      // An unusable candidate must not prevent another face or the generic
      // fallback from opening the console.
    }
  }
  return FALLBACK_FONT;
}
