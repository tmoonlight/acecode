import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const component = fs.readFileSync(
  path.join(here, '..', 'components', 'SlashDropdown.jsx'),
  'utf8',
);

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('slash dropdown applies measured anchored placement before paint', () => {
  assert.match(component, /useLayoutEffect\(\(\) => \{\s*measureLayout\(\);/);
  assert.match(component, /computeAnchoredDropdownLayout\(\{/);
  assert.match(component, /anchorTop:\s*anchorRect\.top/);
  assert.match(component, /anchorBottom:\s*anchorRect\.bottom/);
  assert.match(component, /previousPlacement:\s*measuredPlacementRef\.current/);
  assert.match(component, /measuredPlacementRef\.current = next\.placement/);
  assert.match(component, /data-placement=\{layout\.placement\}/);
  assert.match(component, /top:\s*opensBelow \?/);
  assert.match(component, /bottom:\s*opensBelow \?/);
  assert.match(component, /maxHeight:\s*layout\.maxHeight/);
});

run('slash dropdown coalesces viewport and anchor notifications', () => {
  assert.match(component, /window\.requestAnimationFrame\(\(\) => \{/);
  assert.match(component, /window\.addEventListener\('resize', scheduleMeasureLayout\)/);
  assert.match(component, /window\.addEventListener\('scroll', handleCapturedScroll, true\)/);
  assert.match(component, /visualViewport\?\.addEventListener\?\.\('resize', scheduleMeasureLayout\)/);
  assert.match(component, /visualViewport\?\.addEventListener\?\.\('scroll', scheduleMeasureLayout\)/);
  assert.match(component, /new ResizeObserver\(scheduleMeasureLayout\)/);
  assert.match(component, /resizeObserver\.observe\(anchor\)/);
  assert.match(component, /window\.cancelAnimationFrame\(measureFrameRef\.current\)/);
});

run('slash dropdown does not remeasure direction from its own list changes', () => {
  assert.doesNotMatch(component, /resizeObserver\.observe\(listRef\.current\)/);
  assert.match(component, /if \(event\.target === listRef\.current\) return;/);
  assert.match(component, /onScroll=\{updateScrollMetrics\}/);
  assert.match(
    component,
    /useLayoutEffect\(\(\) => \{\s*updateScrollMetrics\(\);\s*\}, \[[\s\S]*showsAboveIndicator,[\s\S]*showsBelowIndicator,/,
  );
});

run('constrained menu shrinks its scrolling list inside the outer max height', () => {
  assert.match(component, /className="absolute left-0 right-0 flex flex-col/);
  assert.match(component, /className="min-h-0 flex-1 overflow-y-auto"/);
  assert.match(component, /clientHeight:\s*list\.clientHeight/);
  assert.match(component, /visibleEnd = scrollMetrics\.clientHeight > 0/);
});
