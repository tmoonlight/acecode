import assert from 'node:assert/strict';
import {
  nextExpertMenuItemIndex,
  placeExpertSubmenu,
} from './expertMenuPosition.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('expert submenu stays inside a 375px viewport', () => {
  const layout = placeExpertSubmenu({
    anchorRect: { left: 18, right: 226, bottom: 620 },
    menuRect: { height: 228 },
    viewportWidth: 375,
    viewportHeight: 667,
  });
  assert.equal(layout.width, 351);
  assert.ok(layout.left >= 12);
  assert.ok(layout.left + layout.width <= 363);
  assert.ok(layout.top >= 12);
  assert.ok(layout.top + 228 <= 655);
});

test('expert submenu flips left when the right side fits less space', () => {
  const layout = placeExpertSubmenu({
    anchorRect: { left: 500, right: 628, bottom: 460 },
    menuRect: { height: 210 },
    viewportWidth: 640,
    viewportHeight: 480,
  });
  assert.equal(layout.side, 'left');
  assert.ok(layout.left >= 12);
  assert.ok(layout.left + layout.width <= 628);
});

test('expert submenu keyboard navigation wraps and supports boundaries', () => {
  assert.equal(nextExpertMenuItemIndex('ArrowDown', 2, 3), 0);
  assert.equal(nextExpertMenuItemIndex('ArrowUp', 0, 3), 2);
  assert.equal(nextExpertMenuItemIndex('Home', 2, 3), 0);
  assert.equal(nextExpertMenuItemIndex('End', 0, 3), 2);
  assert.equal(nextExpertMenuItemIndex('ArrowDown', 0, 0), -1);
});
