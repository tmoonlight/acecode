import assert from 'node:assert/strict';
import {
  SIDEBAR_TITLE_MARQUEE_END_FADE_PX,
  SIDEBAR_TITLE_MARQUEE_ENDPOINT_HOLD_FRACTION,
  SIDEBAR_TITLE_MARQUEE_SPEED_PX_PER_SECOND,
  SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION,
  sidebarTitleMarqueeMetrics,
} from './sidebarTitleMarquee.js';

function expectedDurationMs(distancePx) {
  return Math.round((
    distancePx / SIDEBAR_TITLE_MARQUEE_SPEED_PX_PER_SECOND
  ) * 1000 / SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION);
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('fitting title stays idle, including subpixel rounding noise', () => {
  assert.deepEqual(sidebarTitleMarqueeMetrics(100, 100), {
    overflowing: false,
    distancePx: 0,
    durationMs: 0,
  });
  assert.equal(sidebarTitleMarqueeMetrics(100.4, 100).overflowing, false);
});

test('invalid or unavailable measurements stay idle', () => {
  for (const [contentWidth, viewportWidth] of [
    [Number.NaN, 100],
    [Number.POSITIVE_INFINITY, 100],
    [-1, 100],
    [100, 0],
    [100, Number.NaN],
  ]) {
    assert.equal(
      sidebarTitleMarqueeMetrics(contentWidth, viewportWidth).overflowing,
      false,
    );
  }
});

test('overflow distance clears the trailing fade so the final character becomes visible', () => {
  const distancePx = 48 + SIDEBAR_TITLE_MARQUEE_END_FADE_PX;
  assert.deepEqual(sidebarTitleMarqueeMetrics(147.2, 100), {
    overflowing: true,
    distancePx,
    durationMs: expectedDurationMs(distancePx),
  });
});

test('endpoint waits and travel fill the complete animation timeline', () => {
  assert.equal(
    (2 * SIDEBAR_TITLE_MARQUEE_ENDPOINT_HOLD_FRACTION)
      + SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION,
    1,
  );
});

test('longer titles take proportionally longer instead of moving faster', () => {
  const short = sidebarTitleMarqueeMetrics(147.2, 100);
  const long = sidebarTitleMarqueeMetrics(356, 100);

  assert.equal(short.durationMs, expectedDurationMs(short.distancePx));
  assert.equal(long.distancePx, 256 + SIDEBAR_TITLE_MARQUEE_END_FADE_PX);
  assert.equal(long.durationMs, expectedDurationMs(long.distancePx));
  assert.ok(long.durationMs > short.durationMs);
});

test('extremely long titles keep the configured travel speed', () => {
  const metrics = sidebarTitleMarqueeMetrics(10000, 100);
  assert.equal(metrics.overflowing, true);
  assert.equal(metrics.durationMs, expectedDurationMs(metrics.distancePx));
});
