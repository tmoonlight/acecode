import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { computeTokenBudgetPanelLayout } from './tokenBudgetPanelLayout.js';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('token ring uses a fixed compact trigger and opens an accessible context panel', () => {
  const component = source('components/TokenBudgetRing.jsx');
  const styles = source('styles/globals.css');
  const triggerStart = styles.indexOf('.ace-token-budget-button');
  const triggerEnd = styles.indexOf('.ace-context-usage-panel', triggerStart);
  const triggerStyles = styles.slice(triggerStart, triggerEnd);

  assert.match(component, /closest\('\.ace-composer-card'\)/);
  assert.match(component, /computeTokenBudgetPanelLayout/);
  assert.match(component, /FULL_CONTEXT_PANEL_WIDTH_PX = 420/);
  assert.match(component, /AGGREGATE_CONTEXT_PANEL_WIDTH_PX = 240/);
  assert.match(component, /Math\.min\(hostRect\.width, preferredWidth\)/);
  assert.match(component, /'ace-context-usage-panel'/);
  assert.match(component, /role="dialog"/);
  assert.match(component, /aria-haspopup="dialog"/);
  assert.match(component, /aria-expanded=\{!!panel\}/);
  assert.match(component, /aria-label="关闭上下文用量"/);
  assert.match(component, /budget\.categories\.map/);
  assert.match(component, /data-context-category=\{category\.tone\}/);
  assert.match(triggerStyles, /--ace-token-budget-size: 20px/);
  assert.doesNotMatch(triggerStyles, /28px/);
});

run('token panel anchors to pointer coordinates, bridges hover, and supports pinned dismissal', () => {
  const component = source('components/TokenBudgetRing.jsx');

  assert.match(component, /HOVER_CLOSE_DELAY_MS = 140/);
  assert.match(component, /x: event\.clientX/);
  assert.match(component, /y: event\.clientY/);
  assert.match(component, /event\.detail > 0/);
  assert.match(component, /onMouseEnter=\{clearCloseTimer\}/);
  assert.match(component, /onMouseLeave=\{scheduleHoverClose\}/);
  assert.match(component, /showPanel\(\s*'click'/);
  assert.match(component, /current\?\.mode === 'click'/);
  assert.match(component, /event\.key === 'Escape'/);
  assert.match(component, /panelRef\.current\?\.contains\(event\.target\)/);
});

run('legacy aggregate usage renders one bar without category placeholder copy', () => {
  const component = source('components/TokenBudgetRing.jsx');
  const aggregateStart = component.indexOf('{aggregateOnly ? (');
  const aggregateEnd = component.indexOf(') : (', aggregateStart);
  const aggregateBranch = component.slice(aggregateStart, aggregateEnd);

  assert.ok(aggregateStart >= 0);
  assert.ok(aggregateEnd > aggregateStart);
  assert.match(component, /const aggregateOnly = known && !budget\?\.breakdownKnown/);
  assert.match(component, /aria-label=\{aggregateOnly \? label : undefined\}/);
  assert.match(component, /'ace-context-usage-panel-aggregate'/);
  assert.match(aggregateBranch, /ace-context-usage-track-aggregate/);
  assert.match(aggregateBranch, /data-context-category="aggregate"/);
  assert.doesNotMatch(
    aggregateBranch,
    /ace-context-usage-(?:header|summary|close|rows|unavailable)/,
  );
  assert.doesNotMatch(component, /当前总用量可用，但本次请求没有分类明细/);
});

run('context panel renders compact values without approximation prefixes', () => {
  const component = source('components/TokenBudgetRing.jsx');

  assert.match(
    component,
    /\{budget\.compactUsedTokens\} \/ \{budget\.compactLimitTokens\} Tokens/,
  );
  assert.match(component, /\{category\.compactTokens\}/);
  assert.doesNotMatch(component, /约/);
});

run('pointer layout keeps the nearest panel edge beside a right-side pointer', () => {
  assert.deepEqual(
    computeTokenBudgetPanelLayout({
      panelWidth: 420,
      pointerX: 989,
      pointerY: 665,
      viewportWidth: 1280,
      viewportHeight: 720,
    }),
    {
      placement: 'above',
      left: 579,
      width: 420,
      maxHeight: 647,
      top: undefined,
      bottom: 65,
    },
  );
});

run('pointer layout clamps a left-side narrow panel inside the viewport', () => {
  const layout = computeTokenBudgetPanelLayout({
    panelWidth: 348,
    pointerX: 5,
    pointerY: 40,
    viewportWidth: 380,
    viewportHeight: 640,
  });

  assert.equal(layout.placement, 'below');
  assert.equal(layout.left, 8);
  assert.equal(layout.width, 348);
  assert.equal(layout.top, 50);
  assert.equal(layout.maxHeight, 582);
});

run('context panel styling is responsive and uses theme tokens', () => {
  const styles = source('styles/globals.css');
  const start = styles.indexOf('.ace-context-usage-panel');
  const end = styles.indexOf('/* WorkBuddy-style composer footer', start);
  const panelStyles = styles.slice(start, end);

  assert.ok(start >= 0);
  assert.ok(end > start);
  assert.match(panelStyles, /position: fixed/);
  assert.match(panelStyles, /max-width: calc\(100vw - 16px\)/);
  assert.match(panelStyles, /background: var\(--ace-surface\)/);
  assert.match(panelStyles, /border: 1px solid var\(--ace-border\)/);
  assert.match(panelStyles, /\.ace-context-usage-panel-aggregate/);
  assert.match(panelStyles, /\.ace-context-usage-track-aggregate\s*\{[^}]*height: 6px/s);
  assert.match(panelStyles, /grid-template-columns: 14px minmax\(0, 1fr\) auto/);
  assert.match(panelStyles, /min-height: 27px/);
  assert.match(panelStyles, /@media \(max-width: 420px\)/);
  assert.match(panelStyles, /\[data-context-category="conversation"\]/);
  assert.doesNotMatch(panelStyles, /#[0-9a-f]{3,8}\b/i);
  assert.doesNotMatch(styles, /\.ace-token-budget-tip/);
});
