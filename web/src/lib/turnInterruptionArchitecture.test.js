import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function section(text, startMarker, endMarker) {
  const start = text.indexOf(startMarker);
  const end = text.indexOf(endMarker, start + startMarker.length);
  assert.ok(start >= 0 && end > start, `missing section ${startMarker}`);
  return text.slice(start, end);
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

test('queue-card and slash interjection both use the immediate interrupt endpoint', () => {
  const chat = source('components/ChatView.jsx');
  const guideFlow = section(
    chat,
    'const guideQueued = useCallback((queuedId) => {',
    '\n\n  const executeBuiltinCommand',
  );
  const slashFlow = section(
    chat,
    "if (route.kind === 'turn_steer') {",
    '\n    const isBuiltin',
  );

  assert.match(guideFlow, /api\.interruptTurn\(targetSid,/);
  assert.doesNotMatch(guideFlow, /api\.steerTurn/);
  assert.match(slashFlow, /api\.interruptTurn\(targetSid, steerPayload\)/);
  assert.doesNotMatch(slashFlow, /api\.steerTurn/);
  assert.ok(
    guideFlow.indexOf('beginQueuedGuidance') < guideFlow.indexOf('api.interruptTurn'),
    'queue item must enter guiding state synchronously before the request starts',
  );
  assert.match(
    guideFlow,
    /queuedItem\?\.queued\?\.state !== QUEUED_INPUT_STATE\.QUEUED/,
    'queue-card duplicates must be blocked from the synchronously updated queue ref',
  );
  assert.ok(
    slashFlow.indexOf('turnInterruptInFlightRef.current.add') <
      slashFlow.indexOf('api.interruptTurn'),
    'slash-command duplicates must be blocked synchronously before the request starts',
  );
});

test('slash interjection and home session creation release only their own request state', () => {
  const chat = source('components/ChatView.jsx');
  const slashFlow = section(
    chat,
    "if (route.kind === 'turn_steer') {",
    '\n    const isBuiltin',
  );
  const homeFlow = section(
    chat,
    "const isBuiltin = !hasExtras && route.kind === 'builtin';",
    '\n    if (composerSubmitting) return;',
  );

  assert.ok(
    slashFlow.indexOf('api.interruptTurn') <
      slashFlow.indexOf('turnInterruptInFlightRef.current.delete(interruptRequestKey)'),
    'slash interjection must release its duplicate-request key after its request settles',
  );
  assert.doesNotMatch(
    homeFlow,
    /interruptRequestKey/,
    'home session creation must not reference slash-interjection request state',
  );
});

test('queue cards expose an edit icon immediately left of interject and save via Modal', () => {
  const list = source('components/QueueCardList.jsx');
  const chat = source('components/ChatView.jsx');
  const queue = source('lib/chatInputQueue.js');
  const card = section(list, 'function QueueCard({', 'export function QueueCardList');
  const dialog = section(list, 'function QueueCardEditDialog({', 'function QueueCard({');

  assert.ok(
    card.indexOf('aria-label="编辑排队消息"') < card.indexOf('aria-label="将排队消息插入当前回合"'),
    'edit SVG must sit immediately left of the interject button',
  );
  assert.match(card, /<VsIcon name="edit"/);
  assert.match(dialog, /<Modal/);
  assert.match(dialog, /labelledBy="queue-card-edit-title"/);
  assert.match(dialog, /编辑排队消息/);
  assert.match(dialog, /aria-label="排队消息内容"/);
  assert.match(chat, /onSaveEdit=\{saveQueuedEdit\}/);
  assert.match(chat, /updateQueuedInputContent\(prev, queuedId, nextText\)/);
  assert.match(queue, /export function updateQueuedInputContent/);
});

test('old-turn idle transition cannot restore or duplicate an accepted interjection', () => {
  const chat = source('components/ChatView.jsx');
  const queue = source('lib/chatInputQueue.js');
  const idleFlow = section(
    chat,
    'const prevBusyRef = useRef(busy);',
    '\n\n  useEffect(() => {\n    if (!sid || items.length === 0)',
  );

  assert.doesNotMatch(chat, /restoreUncommittedGuidanceForSession/);
  assert.doesNotMatch(idleFlow, /finishQueuedGuidance|QUEUED_INPUT_STATE\.QUEUED/);
  assert.match(
    queue,
    /item\.queued\?\.state === QUEUED_INPUT_STATE\.SENDING \|\|\s*item\.queued\?\.state === QUEUED_INPUT_STATE\.GUIDING/,
  );
});
