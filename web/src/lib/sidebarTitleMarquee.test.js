import assert from 'node:assert/strict';
import {
  SIDEBAR_TITLE_MARQUEE_END_FADE_PX,
  SIDEBAR_TITLE_MARQUEE_ENDPOINT_HOLD_FRACTION,
  SIDEBAR_TITLE_MARQUEE_MAX_DURATION_MS,
  SIDEBAR_TITLE_MARQUEE_MIN_DURATION_MS,
  SIDEBAR_TITLE_MARQUEE_SPEED_PX_PER_SECOND,
  SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION,
  sidebarTitleMarqueeMetrics,
} from './sidebarTitleMarquee.js';

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
  assert.deepEqual(sidebarTitleMarqueeMetrics(147.2, 100), {
    overflowing: true,
    distancePx: 48 + SIDEBAR_TITLE_MARQUEE_END_FADE_PX,
    durationMs: SIDEBAR_TITLE_MARQUEE_MIN_DURATION_MS,
  });
});

test('endpoint waits are halved without changing text travel time', () => {
  assert.equal(
    SIDEBAR_TITLE_MARQUEE_MIN_DURATION_MS
      * SIDEBAR_TITLE_MARQUEE_ENDPOINT_HOLD_FRACTION,
    270,
  );
  assert.equal(
    SIDEBAR_TITLE_MARQUEE_MIN_DURATION_MS
      * SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION,
    2520,
  );
  assert.equal(
    SIDEBAR_TITLE_MARQUEE_MAX_DURATION_MS
      * SIDEBAR_TITLE_MARQUEE_ENDPOINT_HOLD_FRACTION,
    1350,
  );
  assert.equal(
    SIDEBAR_TITLE_MARQUEE_MAX_DURATION_MS
      * SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION,
    12600,
  );
});

test('marquee duration scales with readable travel speed between bounds', () => {
  const metrics = sidebarTitleMarqueeMetrics(356, 100);
  const expected = Math.round((
    metrics.distancePx / SIDEBAR_TITLE_MARQUEE_SPEED_PX_PER_SECOND
  ) * 1000 / SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION);

  assert.equal(metrics.distancePx, 256 + SIDEBAR_TITLE_MARQUEE_END_FADE_PX);
  assert.equal(metrics.durationMs, expected);
  assert.ok(metrics.durationMs > SIDEBAR_TITLE_MARQUEE_MIN_DURATION_MS);
  assert.ok(metrics.durationMs < SIDEBAR_TITLE_MARQUEE_MAX_DURATION_MS);
});

test('extremely long titles use the maximum marquee duration', () => {
  const metrics = sidebarTitleMarqueeMetrics(10000, 100);
  assert.equal(metrics.overflowing, true);
  assert.equal(metrics.durationMs, SIDEBAR_TITLE_MARQUEE_MAX_DURATION_MS);
});
