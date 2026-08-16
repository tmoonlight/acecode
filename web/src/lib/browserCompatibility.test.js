import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const componentsRoot = path.join(srcRoot, 'components');
const globalsPath = path.join(srcRoot, 'styles', 'globals.css');
const trajectoryThemePath = path.join(
  componentsRoot,
  'trajectory',
  'deepseek',
  'theme.css',
);
const trajectoryTableCssPath = path.join(
  componentsRoot,
  'trajectory',
  'deepseek',
  'TrajectoryTable.module.css',
);
const trajectoryTimelineCssPath = path.join(
  componentsRoot,
  'trajectory',
  'deepseek',
  'TrajectoryTimeline.module.css',
);
const trajectoryColorMixSupport = '@supports (color: color-mix(in srgb, black, white))';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function walk(dir, exts = new Set(['.css', '.js', '.jsx'])) {
  const out = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      out.push(...walk(full, exts));
    } else if (exts.has(path.extname(entry.name))) {
      out.push(full);
    }
  }
  return out;
}

function stripComments(text) {
  return text
    .replace(/\/\*[\s\S]*?\*\//g, '')
    .replace(/(^|[^:])\/\/.*$/gm, '$1');
}

function cssRuleBody(text, selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const match = text.match(new RegExp(`${escaped}\\s*\\{([^}]*)\\}`, 's'));
  assert.notEqual(match, null, `missing CSS rule: ${selector}`);
  return match[1];
}

function extractCssBlock(text, header) {
  const headerStart = text.indexOf(header);
  assert.notEqual(headerStart, -1, `missing CSS block: ${header}`);
  const bodyStart = text.indexOf('{', headerStart + header.length);
  assert.notEqual(bodyStart, -1, `missing CSS block body: ${header}`);
  let depth = 0;
  for (let index = bodyStart; index < text.length; index += 1) {
    if (text[index] === '{') depth += 1;
    if (text[index] !== '}') continue;
    depth -= 1;
    if (depth === 0) {
      return {
        body: text.slice(bodyStart + 1, index),
        outside: `${text.slice(0, headerStart)}${text.slice(index + 1)}`,
      };
    }
  }
  assert.fail(`unterminated CSS block: ${header}`);
}

run('web source avoids modern color syntax that older WebViews render poorly', () => {
  const guardedDeepSeekTrajectoryCss = new Set([
    path.join('components', 'trajectory', 'deepseek', 'TrajectoryTable.module.css'),
    path.join('components', 'trajectory', 'deepseek', 'TrajectoryTimeline.module.css'),
  ]);
  const forbidden = [
    new RegExp('color-' + 'mix\\s*\\(', 'i'),
    new RegExp('ok' + 'lch\\s*\\(', 'i'),
    new RegExp('\\bok' + 'lab\\b', 'i'),
    new RegExp('\\b' + 'lab\\s*\\(', 'i'),
    new RegExp('\\b' + 'lch\\s*\\(', 'i'),
  ];
  const offenders = [];
  for (const file of walk(srcRoot)) {
    if (path.basename(file) === 'browserCompatibility.test.js') continue;
    const text = stripComments(fs.readFileSync(file, 'utf8'));
    for (const pattern of forbidden) {
      const relative = path.relative(srcRoot, file);
      if (pattern.test(text) && !guardedDeepSeekTrajectoryCss.has(relative)) offenders.push(relative);
    }
  }
  assert.deepEqual([...new Set(offenders)].sort(), []);
});

run('trajectory color-mix formulas are isolated behind feature detection', () => {
  const files = [
    [trajectoryTimelineCssPath, 8],
    [trajectoryTableCssPath, 12],
  ];
  for (const [file, expectedCalls] of files) {
    const css = stripComments(fs.readFileSync(file, 'utf8'));
    const guarded = extractCssBlock(css, trajectoryColorMixSupport);
    assert.doesNotMatch(guarded.outside, /color-mix\s*\(/);
    assert.equal(guarded.body.match(/color-mix\s*\(/g)?.length ?? 0, expectedCalls);
  }
});

run('trajectory WebView 109 fallbacks cover both themes and every consumer', () => {
  const theme = stripComments(fs.readFileSync(trajectoryThemePath, 'utf8'));
  const timeline = extractCssBlock(
    stripComments(fs.readFileSync(trajectoryTimelineCssPath, 'utf8')),
    trajectoryColorMixSupport,
  ).outside;
  const table = extractCssBlock(
    stripComments(fs.readFileSync(trajectoryTableCssPath, 'utf8')),
    trajectoryColorMixSupport,
  ).outside;
  const lightTheme = cssRuleBody(theme, '.ace-trajectory-deepseek-theme');
  const darkThemeMatch = theme.match(
    /\[data-theme='dark'\]\s+\.ace-trajectory-deepseek-theme\s*,\s*\.dark\s+\.ace-trajectory-deepseek-theme\s*\{([^}]*)\}/s,
  );
  assert.notEqual(darkThemeMatch, null, 'missing dark trajectory theme');
  const darkTheme = darkThemeMatch[1];
  const expected = {
    '--dsh-trajectory-fallback-context-color': ['rgb(54, 167, 98)', 'rgb(89, 201, 132)'],
    '--dsh-trajectory-fallback-assistant-color': ['rgb(136, 107, 174)', 'rgb(148, 116, 188)'],
    '--dsh-trajectory-fallback-assistant-ttft-color': ['rgb(191, 175, 211)', 'rgb(100, 83, 123)'],
    '--dsh-trajectory-fallback-hover-outline-color': ['rgba(65, 118, 230, 0.8)', 'rgba(103, 158, 254, 0.8)'],
    '--dsh-trajectory-fallback-selection-color': ['rgba(65, 118, 230, 0.12)', 'rgba(103, 158, 254, 0.12)'],
    '--dsh-trajectory-fallback-selection-dragging-color': ['rgba(65, 118, 230, 0.18)', 'rgba(103, 158, 254, 0.18)'],
    '--dsh-trajectory-fallback-selection-mask-color': ['rgba(255, 255, 255, 0.58)', 'rgba(35, 35, 36, 0.58)'],
    '--dsh-trajectory-fallback-turn-accent-color': ['rgb(212, 228, 253)', 'rgb(40, 56, 82)'],
    '--dsh-trajectory-fallback-request-active-bg': ['rgb(221, 230, 251)', 'rgb(44, 53, 75)'],
    '--dsh-trajectory-fallback-error-rail-bg': ['rgb(251, 203, 203)', 'rgb(81, 47, 48)'],
    '--dsh-trajectory-fallback-turn-label-active-color': ['rgb(91, 131, 198)', 'rgb(110, 152, 218)'],
    '--dsh-trajectory-fallback-diff-added-color': ['rgb(29, 147, 74)', 'rgb(94, 212, 138)'],
    '--dsh-trajectory-fallback-diff-removed-bg': ['rgb(253, 227, 227)', 'rgb(60, 42, 42)'],
    '--dsh-trajectory-fallback-assistant-bg': ['rgb(238, 233, 242)', 'rgb(53, 47, 58)'],
    '--dsh-trajectory-fallback-subtool-color': ['rgb(186, 134, 79)', 'rgb(203, 151, 95)'],
    '--dsh-trajectory-fallback-subtool-bg': ['rgb(254, 249, 241)', 'rgb(37, 36, 33)'],
  };
  for (const [token, [light, dark]] of Object.entries(expected)) {
    assert.ok(lightTheme.includes(`${token}: ${light};`), `missing light fallback ${token}`);
    assert.ok(darkTheme.includes(`${token}: ${dark};`), `missing dark fallback ${token}`);
    assert.match(`${timeline}\n${table}`, new RegExp(`var\\(${token}\\)`));
  }

  assert.match(
    cssRuleBody(table, '.table'),
    /--trajectory-turn-accent:\s*var\(--dsh-trajectory-fallback-turn-accent-color\);/,
  );
  assert.match(
    cssRuleBody(table, '.turnLabelActive'),
    /color:\s*var\(--dsh-trajectory-fallback-turn-label-active-color\);[\s\S]*background:\s*var\(--trajectory-turn-accent\);/,
  );
});

run('trajectory summary headings expose full-row navigation targets', () => {
  const css = stripComments(fs.readFileSync(trajectoryTableCssPath, 'utf8'));
  assert.match(cssRuleBody(css, '.overviewHeading'), /padding:\s*0;/);
  assert.match(cssRuleBody(css, '.overviewTitle'), /width:\s*100%;/);
  assert.match(cssRuleBody(css, '.overviewTitle'), /height:\s*100%;/);
  assert.match(
    cssRuleBody(css, '.overviewTitle:hover'),
    /background:\s*var\(--dsw-alias-interactive-bg-hover\);/,
  );
});

run('color slash-opacity utilities have stable rgba fallbacks', () => {
  const globals = fs.readFileSync(globalsPath, 'utf8');
  const files = [
    ...walk(componentsRoot, new Set(['.jsx'])),
    path.join(srcRoot, 'App.jsx'),
  ];
  const colorSlashUtility = /(?:^|[\s'"`])((?:(?:hover|focus|focus-within):)?(?:bg|border|text|ring)-[A-Za-z0-9_-]+(?:-[A-Za-z0-9_-]+)*\/\d{1,3})(?=[\s'"`])/g;
  const missing = new Set();
  for (const file of files) {
    const text = fs.readFileSync(file, 'utf8');
    for (const match of text.matchAll(colorSlashUtility)) {
      const token = match[1];
      const selector = `.${token.replace(/:/g, '\\:').replace(/\//g, '\\/')}`;
      if (!globals.includes(selector)) {
        missing.add(`${path.relative(srcRoot, file)}:${token}`);
      }
    }
  }
  assert.deepEqual([...missing].sort(), []);
});

run('sidebar blocks static text selection but keeps editing controls selectable', () => {
  const globals = fs.readFileSync(globalsPath, 'utf8');
  assert.match(globals, /\.ace-sidebar\s*\{[^}]*[;\s]user-select:\s*none;/s);
  assert.match(
    globals,
    /\.ace-sidebar input,[\s\S]*?\.ace-sidebar \[contenteditable="true"\]\s*\{[^}]*[;\s]user-select:\s*text;/,
  );
});
