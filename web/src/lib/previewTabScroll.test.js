import assert from 'node:assert/strict';
import {
  previewTabListOverflows,
  scrollLeftForVisibleTab,
} from './previewTabScroll.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('active preview tab remains still when already visible', () => {
  assert.equal(scrollLeftForVisibleTab({
    scrollLeft: 120,
    clientWidth: 300,
    scrollWidth: 900,
    tabOffsetLeft: 180,
    tabOffsetWidth: 100,
  }), 120);
});

run('active preview tab scrolls left into view', () => {
  assert.equal(scrollLeftForVisibleTab({
    scrollLeft: 260,
    clientWidth: 300,
    scrollWidth: 900,
    tabOffsetLeft: 180,
    tabOffsetWidth: 100,
    gutter: 8,
  }), 172);
});

run('active preview tab scrolls right into view', () => {
  assert.equal(scrollLeftForVisibleTab({
    scrollLeft: 0,
    clientWidth: 300,
    scrollWidth: 900,
    tabOffsetLeft: 420,
    tabOffsetWidth: 120,
    gutter: 8,
  }), 248);
});

run('active preview tab scroll clamps to the end', () => {
  assert.equal(scrollLeftForVisibleTab({
    scrollLeft: 0,
    clientWidth: 300,
    scrollWidth: 600,
    tabOffsetLeft: 520,
    tabOffsetWidth: 100,
    gutter: 8,
  }), 300);
});

run('preview tab overflow ignores the one-pixel layout tolerance', () => {
  assert.equal(previewTabListOverflows({ clientWidth: 300, scrollWidth: 300 }), false);
  assert.equal(previewTabListOverflows({ clientWidth: 300, scrollWidth: 301 }), false);
  assert.equal(previewTabListOverflows({ clientWidth: 300, scrollWidth: 302 }), true);
});

run('preview tab overflow requires a measurable viewport', () => {
  assert.equal(previewTabListOverflows({ clientWidth: 0, scrollWidth: 500 }), false);
  assert.equal(previewTabListOverflows({ clientWidth: -10, scrollWidth: 500 }), false);
  assert.equal(previewTabListOverflows({ clientWidth: 300, scrollWidth: 301, tolerance: 0 }), true);
});
