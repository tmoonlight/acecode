import assert from 'node:assert/strict';
import {
  DYNAMIC_LOGO_FPS_PROBE_FRAME_COUNT,
  IDLE_LOGO_LIGHT_RADIUS_PX,
  MAX_LIGHT_DISTANCE_PX,
  MIN_DYNAMIC_LOGO_FPS,
  clampLightToRadius,
  createRandomLogoLightOffset,
  createDynamicLogoFpsProbe,
  interpolateLogoLightOffset,
  isFrameRateBelowMinimum,
} from './interactiveHomeLogoPerformance.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('16 FPS exactly keeps the dynamic logo', () => {
  assert.equal(MIN_DYNAMIC_LOGO_FPS, 16);
  assert.equal(isFrameRateBelowMinimum(0, 1000 / 16), false);
});

test('any valid frame sample below 16 FPS requests static fallback', () => {
  assert.equal(isFrameRateBelowMinimum(0, 1000 / 16 + 0.001), true);
  assert.equal(isFrameRateBelowMinimum(100, 200), true);
});

test('fast, reversed, and invalid frame samples do not request fallback', () => {
  assert.equal(isFrameRateBelowMinimum(0, 1000 / 60), false);
  assert.equal(isFrameRateBelowMinimum(100, 100), false);
  assert.equal(isFrameRateBelowMinimum(100, 99), false);
  assert.equal(isFrameRateBelowMinimum(Number.NaN, 200), false);
});

test('bounded probe turns a 100ms adjacent-frame interval into a low-FPS result', () => {
  assert.equal(DYNAMIC_LOGO_FPS_PROBE_FRAME_COUNT, 2);
  const probe = createDynamicLogoFpsProbe();
  probe.start();
  assert.deepEqual(probe.sample(0), {
    belowMinimum: false,
    framesPerSecond: null,
    shouldContinue: true,
  });
  assert.deepEqual(probe.sample(100), {
    belowMinimum: true,
    framesPerSecond: 10,
    shouldContinue: false,
  });
});

test('probe accepts exactly 16 FPS and resets the idle baseline', () => {
  const probe = createDynamicLogoFpsProbe();
  probe.start();
  probe.sample(0);
  assert.deepEqual(probe.sample(62.5), {
    belowMinimum: false,
    framesPerSecond: 16,
    shouldContinue: false,
  });

  probe.start();
  assert.equal(probe.sample(5000).framesPerSecond, null);
});

test('continuous probe checks every adjacent idle-animation frame', () => {
  const probe = createDynamicLogoFpsProbe();
  probe.start({ continuous: true });
  assert.deepEqual(probe.sample(0), {
    belowMinimum: false,
    framesPerSecond: null,
    shouldContinue: true,
  });
  assert.equal(probe.sample(16).belowMinimum, false);
  assert.deepEqual(probe.sample(116), {
    belowMinimum: true,
    framesPerSecond: 10,
    shouldContinue: false,
  });
});

test('light coordinates inside the 80px radius stay unchanged', () => {
  assert.equal(MAX_LIGHT_DISTANCE_PX, 80);
  assert.deepEqual(clampLightToRadius(30, 40, 0, 0), { x: 30, y: 40 });
  assert.deepEqual(clampLightToRadius(80, 0, 0, 0), { x: 80, y: 0 });
});

test('light coordinates outside the radius clamp along the same direction', () => {
  assert.deepEqual(clampLightToRadius(160, 0, 0, 0), { x: 80, y: 0 });

  const clamped = clampLightToRadius(200, 200, 20, 20);
  assert.ok(
    Math.abs(Math.hypot(clamped.x - 20, clamped.y - 20) - 80) < 1e-9,
  );
  assert.ok(clamped.x > 20 && clamped.y > 20);
});

test('idle light targets stay inside the logo instead of the full 80px clamp', () => {
  assert.equal(IDLE_LOGO_LIGHT_RADIUS_PX, 42);
  const samples = [
    [0, 0],
    [0, 1],
    [0.25, 0.25],
    [0.75, 0.81],
  ];

  for (const [angle, distance] of samples) {
    let index = 0;
    const values = [angle, distance];
    const target = createRandomLogoLightOffset(() => values[index++]);
    assert.ok(
      Math.hypot(target.x, target.y) <= IDLE_LOGO_LIGHT_RADIUS_PX + 1e-9,
    );
  }
});

test('idle light interpolation eases smoothly and clamps progress', () => {
  const start = { x: -40, y: 20 };
  const target = { x: 20, y: -10 };

  assert.deepEqual(interpolateLogoLightOffset(start, target, -1), start);
  assert.deepEqual(interpolateLogoLightOffset(start, target, 0.5), {
    x: -10,
    y: 5,
  });
  assert.deepEqual(interpolateLogoLightOffset(start, target, 2), target);
});
