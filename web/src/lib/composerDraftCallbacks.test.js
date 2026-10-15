import assert from 'node:assert/strict';
import fs from 'node:fs';
import vm from 'node:vm';
import { parseSync, traverse } from '@babel/core';
import { composerDraftEditFingerprint } from './composerDraft.js';
import { inputRouteForPayload } from './builtinCommandRouting.js';

// Execute production receipt guards. A stale response must not modify the
// current editor or its attachment registry, even when its plain text matches.
const source = fs.readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');
const ast = parseSync(source, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const callbacks = new Map();
traverse(ast, {
  VariableDeclarator({ node }) {
    if (node.id.name === 'sendInputOrBuiltin') {
      const callback = node.init.arguments[0];
      callbacks.set('send', source.slice(callback.start, callback.end));
    }
    if (node.id.name === 'clearCurrentSessionDraft') {
      const callback = node.init.arguments[0];
      callbacks.set('clear', source.slice(callback.start, callback.end));
    }
  },
  FunctionDeclaration({ node }) {
    if (node.id.name === 'payloadHasExtras') callbacks.set('hasExtras', source.slice(node.start, node.end));
  },
  CallExpression({ node }) {
    if (node.callee?.name === 'useEffect' && source.slice(node.start, node.end).includes('acceptedHomeSubmission')) {
      callbacks.set('homeCleanup', source.slice(node.arguments[0].start, node.arguments[0].end));
      callbacks.set('homeCleanupDeps', source.slice(node.arguments[1].start, node.arguments[1].end));
    }
    const previous = node.callee?.object;
    if (node.callee?.property?.name === 'then' && previous?.callee?.name === 'createHomeComposerSession') {
      const callback = node.arguments[0];
      callbacks.set('homeReceipt', source.slice(callback.start, callback.end));
    }
    if (node.callee?.property?.name === 'then'
      && previous?.type === 'CallExpression'
      && previous.callee?.object?.name === 'api'
      && previous.callee?.property?.name === 'interruptTurn') {
      const callback = node.arguments[0];
      callbacks.set('turnReceipt', source.slice(callback.start, callback.end));
    }
  },
});
for (const name of ['clear', 'homeReceipt', 'turnReceipt']) assert.ok(callbacks.has(name), `Missing production callback: ${name}`);

function run(name, fn) {
  fn();
  console.log(`[pass] ${name}`);
}

function content({ id = '', leading = false, repeated = false } = {}) {
  const file = { type: 'attachment', key: 'upload-key', id, name: 'notes.md', kind: 'file' };
  const parts = leading
    ? [file, { type: 'text', text: 'before after' }]
    : [{ type: 'text', text: 'before ' }, file, { type: 'text', text: 'after' }];
  if (repeated) parts.push({ ...file });
  return { version: 1, parts };
}

function fixture({
  activeKey = 'workspace:task-a', current = content(), text = 'before after',
  historyText = '/turn before after', routeDisplayText = '/turn before after',
} = {}) {
  const state = { sets: [], saves: [], extraClears: 0, history: [], notifications: [] };
  const context = vm.createContext({
    composerDraftEditFingerprint,
    sid: 'task-a', draftWorkspaceHash: 'workspace', draftSessionKey: 'workspace:task-a',
    draftSessionKeyRef: { current: activeKey }, composerValueRef: { current: text },
    composerContentRef: { current }, draftEditVersionRef: { current: 7 },
    setComposerValue: (value) => state.sets.push(value),
    persistDraftValue: (...args) => { state.saves.push(args); return Promise.resolve(); },
    clearComposerExtras: () => { state.extraClears += 1; },
    recordInputHistory: (value) => state.history.push(value),
    toast: (value) => state.notifications.push(value),
    route: { display_text: routeDisplayText },
    historyText,
    submittedComposerText: 'before after', submittedComposerContent: content(),
    sidRef: { current: activeKey.split(':').at(-1) }, id: 'task-a',
  });
  const clear = vm.runInContext(`(${callbacks.get('clear')})`, context);
  context.clearCurrentSessionDraft = clear;
  return { state, context, clear };
}

function assertUntouched(test) {
  assert.deepEqual(test.state.sets, []);
  assert.deepEqual(test.state.saves, []);
  assert.equal(test.context.draftEditVersionRef.current, 7);
  assert.equal(test.state.extraClears, 0);
}

run('a receipt for another scope preserves a matching current draft and its resources', () => {
  const test = fixture({ activeKey: 'workspace:task-b' });
  assert.equal(test.clear({ expectedText: 'before after', expectedContent: content() }), false);
  assertUntouched(test);
});

run('same-text drafts with a moved file survive the previous send receipt', () => {
  const test = fixture({ current: content({ leading: true }) });
  assert.equal(test.clear({ expectedText: 'before after', expectedContent: content() }), false);
  assertUntouched(test);
});

run('adding a second occurrence is a new draft even though its text and resource ID are unchanged', () => {
  const test = fixture({ current: content({ repeated: true }) });
  assert.equal(test.clear({ expectedText: 'before after', expectedContent: content() }), false);
  assertUntouched(test);
});

run('upload completion can change IDs without preventing the accepted draft from clearing', () => {
  const test = fixture({ current: content({ id: 'uploaded-id' }) });
  assert.equal(test.clear({ expectedText: 'before after', expectedContent: content() }), true);
  assert.deepEqual(test.state.sets, ['']);
  assert.deepEqual(test.state.saves, [['task-a', 'workspace', 'workspace:task-a', '']]);
  assert.equal(test.context.draftEditVersionRef.current, 8);
});

run('a new text edit prevents the earlier receipt from clearing a draft', () => {
  const test = fixture({ text: 'next message' });
  assert.equal(test.clear({ expectedText: 'before after', expectedContent: content() }), false);
  assertUntouched(test);
});

run('the actual turn-interruption receipt retains later edits and their attachment resources', () => {
  const test = fixture({ current: content({ leading: true }) });
  vm.runInContext(`(${callbacks.get('turnReceipt')})`, test.context)();
  assertUntouched(test);
  assert.deepEqual(test.state.history, ['/turn before after']);
});

// 触发场景:运行中的会话里输入 `/turn 按这个改` 并粘贴一段 200 KiB 日志(内联块),插话提交成功。
// 期望行为:cwd 输入历史只记编辑器文本 `/turn 按这个改`,不含内联块正文。
// 回归 bug 表现:历史记的是 route.display_text(已拼上块正文),input_history.jsonl 每条膨胀到
// 几百 KB;上箭头翻回这条时整段(连同 /turn)被折叠成粘贴块,再发送就成了普通消息而不是插话。
run('the actual turn-interruption receipt records editor text without inline paste bodies', () => {
  const test = fixture({
    historyText: '/turn 按这个改',
    routeDisplayText: '/turn 按这个改\n\n<200 KiB log body>',
  });
  vm.runInContext(`(${callbacks.get('turnReceipt')})`, test.context)();
  assert.deepEqual(test.state.history, ['/turn 按这个改']);
});

run('the actual turn-interruption receipt only clears extras after its matching draft clears', () => {
  const test = fixture();
  vm.runInContext(`(${callbacks.get('turnReceipt')})`, test.context)();
  assert.deepEqual(test.state.sets, ['']);
  assert.equal(test.state.extraClears, 1);
});

function homeFixture({ workspace = 'workspace', attachments = false, failSend = false, builtin = false } = {}) {
  const submittedContent = attachments
    ? content() : { version: 1, parts: [{ type: 'text', text: 'before after' }] };
  const test = fixture({ activeKey: '', current: submittedContent });
  const { context, state } = test;
  let previousDeps;
  Object.assign(context, {
    api: {},
    sid: '', draftSessionKey: '', draftReadyKey: '', acceptedHomeSubmission: null,
    sessionCreated: false, createdSessionId: '', pendingAttachmentFiles: [],
    payload: { text: 'before after', composer_content: submittedContent },
    // submit 里算好的 cwd 输入历史文本(只含编辑器文本,不含内联粘贴块正文)。
    historyText: 'before after',
    submittedComposerContent: submittedContent,
    payloadWithAttachmentIds: (payload) => payload,
    setPendingNewSessionFirstUserMessage() {}, applyEvent() {},
    isBuiltin: builtin, explicitHomeSend: !builtin, worktreeIntent: null,
    hasExtras: attachments, hasSwarmMode: builtin,
    composerDirtyRef: { current: true },
    sendInputOrBuiltin: async () => {
      state.sends += 1;
      if (failSend) throw new Error('send failed');
    },
    executeBuiltinCommand: async () => {
      state.directCommands += 1;
      if (failSend) throw new Error('send failed');
    },
    submittedHomeDraftWorkspaceHash: workspace,
    submittedHomeDraftText: { text: 'before after', composer_content: submittedContent },
    onHomeComposerDraftAccepted: () => { state.homeAccepted += 1; },
    setComposerValue: (value) => {
      state.sets.push(value);
      context.composerValueRef.current = value;
      context.composerContentRef.current = null;
    },
    setAcceptedHomeSubmission: (value) => {
      context.acceptedHomeSubmission = typeof value === 'function'
        ? value(context.acceptedHomeSubmission) : value;
    },
  });
  state.homeAccepted = 0;
  state.sends = 0;
  state.directCommands = 0;
  const receipt = vm.runInContext(`(${callbacks.get('homeReceipt')})`, context);
  const render = () => {
    // Before the repair there is no replay effect: running the original receipt
    // and then committing navigation reproduces the nonempty composer.
    if (!callbacks.has('homeCleanup')) return;
    const deps = vm.runInContext(callbacks.get('homeCleanupDeps'), context);
    if (previousDeps && deps.every((dep, index) => Object.is(dep, previousDeps[index]))) return;
    previousDeps = deps;
    vm.runInContext(`(${callbacks.get('homeCleanup')})`, context)();
  };
  const activate = (sid = 'task-a', ready = true) => {
    context.sid = sid;
    context.sidRef.current = sid;
    context.draftWorkspaceHash = workspace;
    context.draftSessionKey = sid ? `${workspace}:${sid}` : '';
    context.draftSessionKeyRef.current = context.draftSessionKey;
    context.draftReadyKey = ready ? context.draftSessionKey : '';
    // useCallback creates a new session-scoped clear function after navigation.
    context.clearCurrentSessionDraft = vm.runInContext(`(${callbacks.get('clear')})`, context);
    render();
  };
  return {
    ...test, render, activate,
    async accept() { await receipt({ id: 'task-a' }); render(); },
    ready() { context.draftReadyKey = context.draftSessionKey; render(); },
  };
}

async function runHome(name, fn) {
  await fn();
  console.log(`[pass] ${name}`);
}

await runHome('home receipt before navigation clears the accepted text once the destination draft is ready', async () => {
  const test = homeFixture();
  await test.accept();
  test.activate('task-a', false);
  assert.deepEqual(test.state.sets, [], 'a not-yet-ready draft must not be consumed');
  test.ready();
  assert.equal(test.context.composerValueRef.current, '', 'accepted home text must not remain after promotion');
  assert.deepEqual(test.state.saves, [['task-a', 'workspace', 'workspace:task-a', '']]);
  assert.equal(test.state.homeAccepted, 1);
  test.render();
  assert.equal(test.state.sets.length, 1, 'acceptance is consumed once');
});

await runHome('navigation before a home receipt clears the accepted text, extras, and saved draft', async () => {
  const test = homeFixture({ attachments: true });
  test.activate();
  assert.deepEqual(test.state.sets, []);
  await test.accept();
  assert.equal(test.context.composerValueRef.current, '');
  assert.equal(test.state.extraClears, 1);
  assert.deepEqual(test.state.saves, [['task-a', 'workspace', 'workspace:task-a', '']]);
});

await runHome('no-workspace home submission also clears after deferred navigation', async () => {
  const test = homeFixture({ workspace: '' });
  await test.accept();
  test.activate();
  assert.equal(test.context.composerValueRef.current, '');
  assert.deepEqual(test.state.saves, [['task-a', '', ':task-a', '']]);
});

await runHome('newer text survives an accepted home receipt and is never cleared by a later render', async () => {
  const test = homeFixture({ attachments: true });
  await test.accept();
  test.context.composerValueRef.current = 'next message';
  test.activate();
  assertUntouched(test);
  assert.equal(test.context.acceptedHomeSubmission, null);
  test.context.composerValueRef.current = 'before after';
  test.render();
  assertUntouched(test);
});

await runHome('moving an inline file while sending preserves the edited draft and resources', async () => {
  const test = homeFixture({ attachments: true });
  test.activate();
  test.context.composerContentRef.current = content({ leading: true });
  await test.accept();
  assertUntouched(test);
});

await runHome('upload metadata completion does not prevent the accepted home draft from clearing', async () => {
  const test = homeFixture({ attachments: true });
  await test.accept();
  test.context.composerContentRef.current = content({ id: 'uploaded-id' });
  test.activate();
  assert.equal(test.context.composerValueRef.current, '');
  assert.equal(test.state.extraClears, 1);
});

await runHome('home receipt never clears an unrelated active task with identical text and files', async () => {
  const test = homeFixture({ attachments: true });
  test.activate('task-b');
  await test.accept();
  assertUntouched(test);
  test.activate();
  assert.equal(test.context.composerValueRef.current, '');
  assert.deepEqual(test.state.saves, [['task-a', 'workspace', 'workspace:task-a', '']]);
});

await runHome('a late destination draft load is cleared only after restoration finishes', async () => {
  const test = homeFixture();
  test.activate('task-a', false);
  test.context.composerValueRef.current = '';
  await test.accept();
  assert.deepEqual(test.state.sets, []);
  test.context.composerValueRef.current = 'before after';
  test.ready();
  assert.equal(test.context.composerValueRef.current, '');
  assert.equal(test.state.saves.length, 1);
});

await runHome('a failed home send preserves the submitted text, resources, and home draft', async () => {
  const test = homeFixture({ attachments: true, failSend: true });
  await assert.rejects(test.accept(), /send failed/);
  test.activate();
  assertUntouched(test);
  assert.equal(test.context.acceptedHomeSubmission, null);
  assert.equal(test.context.composerValueRef.current, 'before after');
  assert.equal(test.state.homeAccepted, 0);
});

// 触发：在首页同时选择 Goal 与网状蜂群后发送。旧分支保留输入却跳过 builtin 回执清理。
for (const navigationFirst of [false, true]) {
  await runHome(`home Goal with swarm clears its draft (navigationFirst=${navigationFirst})`, async () => {
    const test = homeFixture({ builtin: true, workspace: '' });
    if (navigationFirst) test.activate();
    await test.accept();
    if (!navigationFirst) test.activate();
    assert.equal(test.context.composerValueRef.current, '');
    assert.equal(test.state.extraClears, 1);
    assert.equal(test.state.sends, 1, 'home commands must use the shared sender');
    assert.equal(test.state.directCommands, 0);
    assert.equal(test.state.homeAccepted, 1);
  });
}

await runHome('home Goal failure retains the input and does not consume either draft', async () => {
  const test = homeFixture({ builtin: true, failSend: true });
  await assert.rejects(test.accept(), /send failed/);
  test.activate();
  assertUntouched(test);
  assert.equal(test.state.homeAccepted, 0);
});

await runHome('a successful home Goal receipt preserves newer edits', async () => {
  const test = homeFixture({ builtin: true });
  await test.accept();
  test.context.composerValueRef.current = 'next objective';
  test.activate();
  assertUntouched(test);
});

function senderFixture(execute) {
  const sent = [];
  const context = vm.createContext({
    inputRouteForPayload,
    executeBuiltinCommand: execute,
    materializeWorkspaceDraftPastes: async (_sid, payload) => payload,
    api: { sendInput: async (sid, payload) => { sent.push({ sid, payload }); } },
  });
  vm.runInContext(callbacks.get('hasExtras'), context);
  return { send: vm.runInContext(`(${callbacks.get('send')})`, context), sent };
}

// 触发：Goal 走命令接口，旧实现只发送 route.command，丢掉 payload.swarm_mode。
for (const mode of ['mesh', 'star', 'off']) {
  await runHome(`Goal waits for ${mode} selection before starting`, async () => {
    const calls = [];
    let acceptMode;
    const modeReceipt = new Promise((resolve) => { acceptMode = resolve; });
    const test = senderFixture(async (sid, command) => {
      calls.push({ sid, ...command });
      if (command.command === 'swarm') await modeReceipt;
    });
    const pending = test.send('task-a', { text: '/goal test collaboration', swarm_mode: mode });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].command, 'swarm');
    assert.equal(calls[0].args, mode);
    acceptMode();
    await pending;
    assert.equal(calls.length, 2);
    assert.equal(calls[1].sid, 'task-a');
    assert.equal(calls[1].command, 'goal');
    assert.equal(calls[1].args, 'test collaboration');
    assert.equal(test.sent.length, 0);
  });
}

await runHome('failed swarm selection prevents Goal from starting', async () => {
  const calls = [];
  const test = senderFixture(async (_sid, command) => {
    calls.push(command.command);
    throw new Error('tree still running');
  });
  await assert.rejects(test.send('task-a', { text: '/goal test', swarm_mode: 'off' }), /tree still running/);
  assert.deepEqual(calls, ['swarm']);
});

await runHome('Goal without a local swarm choice preserves the server mode', async () => {
  const calls = [];
  const test = senderFixture(async (_sid, command) => { calls.push(command.command); });
  await test.send('task-a', { text: '/goal test' });
  assert.deepEqual(calls, ['goal']);
});

await runHome('explicit swarm commands override the composer choice', async () => {
  const calls = [];
  const test = senderFixture(async (_sid, command) => { calls.push(command); });
  await test.send('task-a', { text: '/swarm off', swarm_mode: 'mesh' });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].command, 'swarm');
  assert.equal(calls[0].args, 'off');
});

await runHome('ordinary swarm messages still use the message endpoint', async () => {
  const test = senderFixture(async () => { assert.fail('ordinary input must not execute commands'); });
  const payload = { text: 'test', swarm_mode: 'mesh' };
  await test.send('task-a', payload);
  assert.deepEqual(test.sent, [{ sid: 'task-a', payload }]);
});
