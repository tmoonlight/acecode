import assert from 'node:assert/strict';
import {
  IDLE_LOGO_LIGHT_RADIUS_PX,
  MAX_LIGHT_DISTANCE_PX,
  clampLightToRadius,
  createRandomLogoLightOffset,
  interpolateLogoLightOffset,
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
