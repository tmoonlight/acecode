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
    'const guideQueued = useStableCallback((queuedId) => {',
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
  assert.doesNotMatch(dialog, /Ctrl\+Enter 保存/);
  assert.match(dialog, /<span>保存<\/span>[\s\S]*<span[^>]*>Ctrl \+ Enter<\/span>/);
  assert.match(chat, /onSaveEdit=\{saveQueuedEdit\}/);
  assert.match(chat, /updateQueuedInputContent\(prev, queuedId, nextText, \{ composerContent \}\)/);
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

// 场景:模型正在回复时,用户在输入框里按 Enter / Ctrl+Enter / Ctrl+Alt+Enter,
// 或点对话末尾插话气泡上的「立即发送」/ 撤回(与 Claude 桌面版 next / later / now 三档对应)。
// 期望:Enter 先入队再以软插话(/turn/steer)插进当前回合,气泡画在对话末尾;Ctrl+Enter
// 只入队、回合结束后发;Ctrl+Alt+Enter 复制整个会话(fork whole)并在新会话发送;
// 「立即发送」先标记 now 再调 send-now,撤回调 withdraw;回合结束时没被读到的插话转回排队,
// 且暂停判断读最新的队列状态。
// 回归:曾经回复中 Enter 一律排队,要等回合结束才发出去。
test('busy composer sends steer on Enter, queues on Ctrl+Enter and forks on Ctrl+Alt+Enter', () => {
  const chat = source('components/ChatView.jsx');
  const submit = section(chat, 'const submit = useStableCallback((text, { sendMode', 'const isBuiltin =');
  assert.match(submit, /if \(sendMode === 'fork' && route\.kind === 'message'\) \{\s*sendInForkedSession\(/);
  const busyBranch = section(chat, 'if ((liveTranscript.busy || liveTranscript.abortPending) && !isBuiltin) {', 'const targetSid = sid;');
  assert.match(busyBranch, /const queuedId = enqueueInput\(payload\);/);
  assert.match(busyBranch, /sendMode !== 'queue' && liveTranscript\.busy && !liveTranscript\.abortPending && steerTurnId/);
  assert.match(busyBranch, /steerQueuedInput\(queuedId, steerTurnId\);/);

  const steer = section(chat, 'const steerQueuedInput = useStableCallback(', 'const sendSteeredNow = useStableCallback(');
  assert.match(steer, /beginQueuedGuidance\(prev, queuedId, \{ turnId: expectedTurnId, lane: STEER_LANE \}\)/);
  assert.match(steer, /api\.steerTurn\(targetSid, \{ \.\.\.requestPayload, expected_turn_id: expectedTurnId \}\)/);
  assert.match(steer, /finishQueuedGuidance\(prev, queuedId, \{ succeeded: false \}\)/);

  const sendNow = section(chat, 'const sendSteeredNow = useStableCallback(', 'const withdrawSteered = useStableCallback(');
  assert.ok(sendNow.indexOf('markQueuedSteerSendingNow') < sendNow.indexOf('api.sendSteerNow('),
    '先标记立即发送再发请求,回合结束的回退才不会把它重发');
  assert.match(chat, /api\.withdrawSteer\(targetSid, queuedId\)/);

  assert.match(chat, /if \(wasBusy && !busy\) releaseSteersAfterTurn\(\);/);
  assert.match(chat, /shouldPauseQueuedInputAfterAbort\(\{\s*state: queueStore\.getState\(\),/);
  assert.match(chat, /<SteeredMessages items=\{steeredItems\} onSendNow=\{sendSteeredNow\} onWithdraw=\{withdrawSteered\} \/>/);

  const fork = section(chat, 'const sendInForkedSession = useStableCallback(', 'useEffect(() => {');
  assert.match(fork, /api\.forkWholeSession\(sourceSid\)/);
  assert.ok(fork.indexOf('sendInputOrBuiltin(r.session_id, payload)') < fork.indexOf('promoteForkedSession(r)'),
    '先在新会话发出消息,再切过去');
  assert.match(chat, /forkSendAvailable=\{!!sid && !readOnlyExternalSession && transcriptLoadState === 'loaded'\}/);
});
