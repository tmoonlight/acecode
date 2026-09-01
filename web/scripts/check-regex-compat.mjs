#!/usr/bin/env node
// Scans the built frontend bundle (web/dist/index.html) for regex literals
// that use lookbehind assertions `(?<=...)` / `(?<!...)`.
//
// Legacy WebKit engines (Safari < 16.4, e.g. the WKWebView on macOS 12)
// cannot compile lookbehind. Regex literals are compiled when the enclosing
// module script is parsed, so a single incompatible literal makes the whole
// bundle fail and the app renders a blank screen.
//
// Run automatically after `pnpm build`. Exits non-zero when a lookbehind
// regex literal is found.

import { readFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const distPath = resolve(root, 'dist/index.html');

let html;
try {
  html = readFileSync(distPath, 'utf8');
} catch {
  console.error(`[check-regex-compat] cannot read ${distPath}; run \`pnpm build\` first`);
  process.exit(1);
}

// Tokenizer that walks the bundle and extracts regex literals while skipping
// strings, template literals and comments. Regex-vs-division is decided with
// the usual preceding-token heuristic.
const REGEX_PRECEDING = new Set('(,=:[!&|?+*-%^<>~;{}`'.split(''));
const KEYWORDS = new Set([
  'return', 'case', 'typeof', 'in', 'of', 'new', 'delete', 'void',
  'instanceof', 'do', 'else', 'yield', 'await', 'throw',
]);

function extractRegexLiterals(src) {
  const found = [];
  const n = src.length;
  let i = 0;

  const prevSignificant = (idx) => {
    let j = idx - 1;
    while (j >= 0 && (src[j] === ' ' || src[j] === '\t' || src[j] === '\r' || src[j] === '\n')) j--;
    return j >= 0 ? src[j] : '';
  };
  const prevIsKeyword = (idx) => {
    let j = idx - 1;
    while (j >= 0 && /[A-Za-z0-9_$]/.test(src[j])) j--;
    return KEYWORDS.has(src.slice(j + 1, idx));
  };

  while (i < n) {
    const c = src[i];
    if (c === '/' && src[i + 1] === '/') { i = src.indexOf('\n', i) + 1 || n; continue; }
    if (c === '/' && src[i + 1] === '*') { i = (src.indexOf('*/', i + 2) + 2) || n; continue; }
    if (c === '"' || c === "'") {
      i++;
      while (i < n && src[i] !== c && src[i] !== '\n') i += src[i] === '\\' ? 2 : 1;
      i++;
      continue;
    }
    if (c === '`') {
      i++;
      while (i < n && src[i] !== '`') {
        if (src[i] === '\\') { i += 2; continue; }
        if (src[i] === '$' && src[i + 1] === '{') {
          let depth = 1;
          i += 2;
          while (i < n && depth > 0) {
            if (src[i] === '\\') { i += 2; continue; }
            if (src[i] === '{') depth++;
            else if (src[i] === '}') depth--;
            i++;
          }
          continue;
        }
        i++;
      }
      i++;
      continue;
    }
    if (c === '/' && (prevSignificant(i) === '' || REGEX_PRECEDING.has(prevSignificant(i)) || prevIsKeyword(i))) {
      let j = i + 1;
      let inClass = false;
      let closed = false;
      while (j < n) {
        const ch = src[j];
        if (ch === '\\') { j += 2; continue; }
        if (ch === '\n') break;
        if (ch === '[') inClass = true;
        else if (ch === ']') inClass = false;
        else if (ch === '/' && !inClass) { closed = true; break; }
        j++;
      }
      if (closed) {
        let k = j + 1;
        while (k < n && /[a-z]/i.test(src[k])) k++;
        found.push({ index: i, pattern: src.slice(i + 1, j), flags: src.slice(j + 1, k) });
        i = k;
        continue;
      }
    }
    i++;
  }
  return found;
}

const literals = extractRegexLiterals(html);
const offenders = literals.filter(({ pattern }) => /\(\?<[=!]/.test(pattern));

console.log(`[check-regex-compat] scanned ${literals.length} regex literals in dist/index.html`);
if (offenders.length > 0) {
  console.error(`[check-regex-compat] FAIL: ${offenders.length} regex literal(s) use lookbehind, which breaks legacy WebKit (Safari < 16.4):`);
  for (const { pattern, flags } of offenders.slice(0, 10)) {
    console.error(`  /${pattern.slice(0, 120)}/${flags}`);
  }
  process.exit(1);
}
console.log('[check-regex-compat] OK: no lookbehind regex literals found');
