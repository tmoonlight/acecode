import assert from 'node:assert/strict';
import {
  captureRichComposerContextSelection,
  insertRichComposerContextText,
  normalizeRichComposerContextSelection,
  RICH_COMPOSER_CONTEXT_PASTE_ACTIONS,
  RICH_COMPOSER_CONTEXT_PASTE_EVENT,
} from './richComposerContextPaste.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function eventHarness(listener) {
  const seen = [];
  return {
    target: {
      dispatchEvent(event) {
        seen.push(event);
        listener?.(event);
        return true;
      },
    },
    createEvent(type, init) {
      return { type, detail: init.detail };
    },
    seen,
  };
}

run('context selection normalization clamps offsets and direction', () => {
  assert.deepEqual(normalizeRichComposerContextSelection({
    start: -4.8,
    end: 9.9,
    direction: 'backward',
  }), {
    start: 0,
    end: 9,
    direction: 'backward',
  });
  assert.deepEqual(normalizeRichComposerContextSelection({
    start: 5,
    end: 2,
    direction: 'sideways',
  }), {
    start: 5,
    end: 5,
    direction: 'none',
  });
  assert.equal(normalizeRichComposerContextSelection({ start: 'x', end: 2 }), null);
});

run('capture dispatches once and returns the handled composer selection', () => {
  const harness = eventHarness((event) => {
    assert.equal(event.type, RICH_COMPOSER_CONTEXT_PASTE_EVENT);
    assert.equal(event.detail.action, RICH_COMPOSER_CONTEXT_PASTE_ACTIONS.CAPTURE_SELECTION);
    event.detail.selection = { start: 3, end: 7, direction: 'forward' };
    event.detail.handled = true;
  });

  assert.deepEqual(captureRichComposerContextSelection(harness.target, {
    createEvent: harness.createEvent,
  }), {
    start: 3,
    end: 7,
    direction: 'forward',
  });
  assert.equal(harness.seen.length, 1);
});

run('capture returns null when the target does not handle the event', () => {
  const harness = eventHarness();
  assert.equal(captureRichComposerContextSelection(harness.target, {
    createEvent: harness.createEvent,
  }), null);
  assert.equal(harness.seen.length, 1);
});

run('insert dispatches exact text and normalized selection once', () => {
  let received = null;
  const harness = eventHarness((event) => {
    received = { ...event.detail };
    event.detail.handled = true;
  });

  assert.equal(insertRichComposerContextText(
    harness.target,
    'line 1\r\nline 2',
    { start: 2, end: 5, direction: 'backward' },
    { createEvent: harness.createEvent },
  ), true);
  assert.equal(harness.seen.length, 1);
  assert.deepEqual(received, {
    action: RICH_COMPOSER_CONTEXT_PASTE_ACTIONS.INSERT_TEXT,
    handled: false,
    text: 'line 1\r\nline 2',
    selection: { start: 2, end: 5, direction: 'backward' },
  });
});

run('insert reports unhandled targets so generic controls can fall back', () => {
  const harness = eventHarness();
  assert.equal(insertRichComposerContextText(
    harness.target,
    'hello',
    null,
    { createEvent: harness.createEvent },
  ), false);
  assert.equal(insertRichComposerContextText(null, 'hello', null, {
    createEvent: harness.createEvent,
  }), false);
});
