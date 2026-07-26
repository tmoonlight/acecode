import assert from 'node:assert/strict';
import {
  selectionActionPopoverPosition,
  selectionPointerViewportRect,
  selectionRangeViewportRect,
} from './selectionActionPopover.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('selection action popover centers below an ordinary selection', () => {
  assert.deepEqual(
    selectionActionPopoverPosition(
      { left: 100, top: 40, bottom: 60, width: 80 },
      { width: 180, height: 36 },
      { width: 800, height: 600 },
    ),
    { left: 50, top: 68, placement: 'below' },
  );
});

run('selection action popover flips above when the lower edge is crowded', () => {
  assert.deepEqual(
    selectionActionPopoverPosition(
      { left: 300, top: 540, bottom: 560, width: 60 },
      { width: 180, height: 48 },
      { width: 800, height: 600 },
    ),
    { left: 240, top: 484, placement: 'above' },
  );
});

run('selection action popover clamps to viewport margins', () => {
  assert.deepEqual(
    selectionActionPopoverPosition(
      { left: -20, top: 5, bottom: 15, width: 10 },
      { width: 200, height: 70 },
      { width: 240, height: 100 },
    ),
    { left: 8, top: 22, placement: 'below' },
  );
});

run('cursor-anchored popover opens above and beside the released pointer', () => {
  assert.deepEqual(
    selectionActionPopoverPosition(
      { left: 420, top: 300, right: 420, bottom: 300, width: 0, height: 0 },
      { width: 180, height: 48 },
      { width: 800, height: 600 },
      { anchorMode: 'cursor' },
    ),
    { left: 428, top: 244, placement: 'above' },
  );
});

run('cursor-anchored popover flips left instead of escaping the viewport', () => {
  assert.deepEqual(
    selectionActionPopoverPosition(
      { left: 780, top: 300, right: 780, bottom: 300, width: 0, height: 0 },
      { width: 180, height: 48 },
      { width: 800, height: 600 },
      { anchorMode: 'cursor' },
    ),
    { left: 592, top: 244, placement: 'above' },
  );
});

run('pointer viewport rect preserves the mouseup cursor position', () => {
  assert.deepEqual(selectionPointerViewportRect({ clientX: 321, clientY: 456 }), {
    left: 321,
    top: 456,
    right: 321,
    bottom: 456,
    width: 0,
    height: 0,
  });
  assert.equal(selectionPointerViewportRect({ clientX: undefined, clientY: 10 }), null);
});

run('keyboard fallback uses the first visible range fragment instead of a multiline union', () => {
  const first = { left: 120, top: 50, right: 210, bottom: 68, width: 90, height: 18 };
  const selection = {
    rangeCount: 1,
    isCollapsed: false,
    getRangeAt() {
      return {
        getClientRects: () => [
          first,
          { left: 40, top: 70, right: 760, bottom: 88, width: 720, height: 18 },
        ],
        getBoundingClientRect: () => ({
          left: 40,
          top: 50,
          right: 760,
          bottom: 88,
          width: 720,
          height: 38,
        }),
      };
    },
  };
  assert.deepEqual(selectionRangeViewportRect(selection), first);
});
