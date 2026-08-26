import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const styles = fs.readFileSync(path.join(srcRoot, 'styles', 'globals.css'), 'utf8');

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function rule(selector) {
  const escaped = selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  const match = styles.match(new RegExp(`${escaped}\\s*\\{([\\s\\S]*?)\\}`));
  assert.ok(match, `missing CSS rule: ${selector}`);
  return match[1];
}

run('todo dock reuses the change dock glass material', () => {
  assert.match(
    styles,
    /\.ace-change-glass-dock,\s*\.ace-todo-glass-dock\s*\{[\s\S]*?border: 1px solid rgba\(var\(--ace-border-rgb\), 0\.58\);[\s\S]*?background: var\(--ace-surface\);[\s\S]*?box-shadow: var\(--ace-shadow\);/,
  );
  assert.match(
    styles,
    /@supports \(\(-webkit-backdrop-filter: blur\(1px\)\) or \(backdrop-filter: blur\(1px\)\)\) \{[\s\S]*?\.ace-change-glass-dock,\s*\.ace-todo-glass-dock\s*\{[\s\S]*?background: rgba\(var\(--ace-surface-rgb\), 0\.58\);[\s\S]*?backdrop-filter: blur\(8px\) saturate\(1\.08\);/,
  );
  assert.match(
    styles,
    /\.ace-change-glass-dock::before,\s*\.ace-todo-glass-dock::before\s*\{[\s\S]*?background: rgba\(var\(--ace-surface-rgb\), 0\.36\);/,
  );
});

run('todo content stays above the shared glass tint', () => {
  const content = rule('.ace-todo-glass-content');
  assert.match(content, /position:\s*relative;/);
  assert.match(content, /z-index:\s*2;/);
  assert.doesNotMatch(styles, /--ace-todo-(?:dock-bg|dock-border|shadow):/);
});
