import assert from 'node:assert/strict';
import {
  activityAnchorViewportTop,
  matchesProgrammaticActivityScroll,
  scrollTopForPreservedActivityAnchor,
} from './activityExpansionAnchor.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('activity anchor top is measured relative to the transcript viewport', () => {
  assert.equal(activityAnchorViewportTop({ containerTop: 100, anchorTop: 364 }), 264);
});

run('activity anchor compensation preserves its original viewport position', () => {
  assert.equal(scrollTopForPreservedActivityAnchor({
    scrollTop: 400,
    anchorViewportTop: 260,
    currentAnchorViewportTop: 340,
    clientHeight: 500,
    scrollHeight: 1400,
  }), 480);
});

run('activity anchor compensation clamps at the transcript top', () => {
  assert.equal(scrollTopForPreservedActivityAnchor({
    scrollTop: 30,
    anchorViewportTop: 220,
    currentAnchorViewportTop: 120,
    clientHeight: 500,
    scrollHeight: 1400,
  }), 0);
});

run('activity anchor compensation clamps at the transcript tail', () => {
  assert.equal(scrollTopForPreservedActivityAnchor({
    scrollTop: 700,
    anchorViewportTop: 120,
    currentAnchorViewportTop: 300,
    clientHeight: 500,
    scrollHeight: 1300,
  }), 800);
});

run('programmatic scroll matching allows subpixel browser rounding', () => {
  assert.equal(matchesProgrammaticActivityScroll(480.4, 480, 0.5), true);
  assert.equal(matchesProgrammaticActivityScroll(481, 480, 0.5), false);
  assert.equal(matchesProgrammaticActivityScroll(480, null), false);
});
