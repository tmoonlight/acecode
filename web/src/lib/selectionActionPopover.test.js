import assert from 'node:assert/strict';
import { selectionActionPopoverPosition } from './selectionActionPopover.js';

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
