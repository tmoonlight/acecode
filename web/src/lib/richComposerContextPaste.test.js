import assert from 'node:assert/strict';
import {
  captureRichComposerContextSelection,
  insertRichComposerContextText,
  normalizeRichComposerContextSelection,
  RICH_COMPOSER_CONTEXT_PASTE_ACTIONS,
  RICH_COMPOSER_CONTEXT_PASTE_EVENT,
  RICH_COMPOSER_ROOT_ATTRIBUTE,
  richComposerRootFromTarget,
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
      return { type, ...init };
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

run('composer root lookup climbs from a Slate leaf and ignores unrelated editables', () => {
  const root = {
    parentElement: null,
    getAttribute(name) {
      return name === RICH_COMPOSER_ROOT_ATTRIBUTE ? 'true' : null;
    },
  };
  const leaf = {
    parentElement: root,
    getAttribute() { return null; },
  };
  const unrelated = {
    parentElement: null,
    getAttribute() { return null; },
  };

  assert.equal(richComposerRootFromTarget(leaf), root);
  assert.equal(richComposerRootFromTarget(unrelated), null);
});

run('capture dispatches once and returns the handled composer selection', () => {
  const harness = eventHarness((event) => {
    assert.equal(event.type, RICH_COMPOSER_CONTEXT_PASTE_EVENT);
    assert.equal(event.bubbles, true);
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

run('child dispatch bubbles once to the owning composer listener', () => {
  let rootDispatchCount = 0;
  const root = {
    dispatchEvent(event) {
      rootDispatchCount += 1;
      event.detail.selection = { start: 4, end: 4, direction: 'none' };
      event.detail.handled = true;
      return true;
    },
  };
  const child = {
    dispatchEvent(event) {
      if (event.bubbles) root.dispatchEvent(event);
      return true;
    },
  };
  const createEvent = (type, init) => ({ type, ...init });

  assert.deepEqual(captureRichComposerContextSelection(child, { createEvent }), {
    start: 4,
    end: 4,
    direction: 'none',
  });
  assert.equal(rootDispatchCount, 1);
});
