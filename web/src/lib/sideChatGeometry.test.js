import assert from 'node:assert/strict';
import {
  anchorSideChatGeometry,
  clampSideChatGeometry,
  createSideChatGeometry,
  moveSideChatGeometry,
  resizeSideChatGeometry,
} from './sideChatGeometry.js';

const viewport = { left: 0, top: 0, width: 1280, height: 900 };
const initial = createSideChatGeometry(viewport);
assert.deepEqual(initial, { left: 340, top: 140, width: 600, height: 620 });

// Toolbar activation aligns top-left where it fits and preserves resized dimensions.
assert.deepEqual(anchorSideChatGeometry(initial, { left: 120, top: 40 }, viewport), {
  left: 120, top: 40, width: 600, height: 620,
});
assert.deepEqual(anchorSideChatGeometry(initial, { left: 1200, top: 850 }, viewport), {
  left: 668, top: 628, width: 600, height: 260,
});
assert.deepEqual(anchorSideChatGeometry({ ...initial, width: 420, height: 360 }, { left: 120, top: 40 }, viewport), {
  left: 120, top: 40, width: 420, height: 360,
});
assert.deepEqual(anchorSideChatGeometry(initial, { left: 420, top: 210 }, { left: 73, top: 180, width: 390, height: 350 }), {
  left: 85, top: 210, width: 366, height: 308,
});
assert.deepEqual(anchorSideChatGeometry(initial, { left: 340, top: 56 }, { width: 390, height: 600 }), {
  left: 12, top: 56, width: 366, height: 532,
});

assert.deepEqual(moveSideChatGeometry(initial, 9000, -9000, viewport), {
  left: 668, top: 12, width: 600, height: 620,
});

// Opposite edges stay anchored while west/north edges stop at the minimum size.
const west = resizeSideChatGeometry(initial, 'w', 9000, 0, viewport);
assert.equal(west.width, 300);
assert.equal(west.left + west.width, initial.left + initial.width);
const north = resizeSideChatGeometry(initial, 'n', 0, 9000, viewport);
assert.equal(north.height, 260);
assert.equal(north.top + north.height, initial.top + initial.height);
const northWest = resizeSideChatGeometry(initial, 'nw', -9000, -9000, viewport);
assert.deepEqual(northWest, { left: 12, top: 12, width: 928, height: 748 });

const southEast = resizeSideChatGeometry(initial, 'se', 9000, 9000, viewport);
assert.deepEqual(southEast, { left: 340, top: 140, width: 928, height: 748 });

// Narrow viewports and keyboard/pinch-zoom offsets keep the full window visible.
assert.deepEqual(clampSideChatGeometry(initial, { width: 280, height: 230 }), {
  left: 12, top: 12, width: 256, height: 206,
});
assert.deepEqual(createSideChatGeometry({ left: 73, top: 180, width: 390, height: 350 }), {
  left: 85, top: 192, width: 366, height: 326,
});

for (const width of [0, 20, 280, 390, 1280, 3000]) {
  for (const height of [0, 20, 180, 350, 900, 1600]) {
    const view = { left: 17, top: 41, width, height };
    const fitted = createSideChatGeometry(view);
    assert.deepEqual(clampSideChatGeometry(fitted, view), fitted);
    for (const direction of ['n', 'e', 's', 'w', 'ne', 'se', 'sw', 'nw']) {
      for (const delta of [-9000, -100, 0, 100, 9000]) {
        const rect = resizeSideChatGeometry(fitted, direction, delta, delta, view);
        assert.ok(Object.values(rect).every(Number.isFinite));
        assert.ok(rect.width >= 0 && rect.height >= 0);
        assert.ok(rect.left >= view.left && rect.top >= view.top);
        assert.ok(rect.left + rect.width <= view.left + view.width);
        assert.ok(rect.top + rect.height <= view.top + view.height);
        if (!direction.includes('n')) assert.equal(rect.top, fitted.top);
        if (!direction.includes('w')) assert.equal(rect.left, fitted.left);
        if (!direction.includes('e')) assert.equal(rect.left + rect.width, fitted.left + fitted.width);
        if (!direction.includes('s')) assert.equal(rect.top + rect.height, fitted.top + fitted.height);
      }
    }
  }
}

assert.deepEqual(clampSideChatGeometry({ left: NaN, top: Infinity, width: -1, height: Infinity }, viewport), {
  left: 12, top: 12, width: 300, height: 620,
});
console.log('sideChatGeometry.test.js: all tests passed');
