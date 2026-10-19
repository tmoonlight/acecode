import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import { createTranscriptState, reconcileTranscriptWithServer, reduceTranscriptEvent } from './sessionTranscript.js';

// 复现反馈:页面停在「正在推理」,daemon 早已收尾(busy_changed(false) / done 没被页面
// 处理),切走再切回才看到完成。这里分两层验证兜底:
//   1. reconcileTranscriptWithServer 纯函数:服务端说空闲时,卡住的运行态必须被清掉,
//      不能被「防回退」保护原样留下;
//   2. 用生产 hook 跑一遍:运行中 20 秒没事件 → 探针 → 服务端空闲 → 重拉历史 → 从服务端
//      当前 seq 重新订阅,并上报诊断。

function stuckState() {
  let state = createTranscriptState({ isLive: true, loadState: 'loaded' });
  const events = [
    { type: 'message', seq: 1, payload: { id: 'u1', role: 'user', content: '帮我看看' } },
    { type: 'busy_changed', seq: 2, payload: { busy: true, turn_id: 't1' } },
    { type: 'agent_progress', seq: 3, payload: { phase: 'reasoning', label: '正在推理' } },
    { type: 'reasoning', seq: 4, payload: { text: '思考中' } },
  ];
  for (const event of events) state = reduceTranscriptEvent(state, event).state;
  return state;
}

// 场景:页面卡在第 4 帧(正在推理),服务端快照显示回合已结束、最终回答已落盘。
// 期望:对账后不再运行(busy/activity/activeTurnId 清空),最终回答可见,lastSeq 取服务端
// 当前 seq,之后从这里重新订阅不会再回放已体现在历史里的旧事件。
{
  const stuck = stuckState();
  assert.equal(stuck.busy, true);
  assert.equal(stuck.activity.label, '正在推理');
  const data = {
    messages: [
      { id: 'u1', role: 'user', content: '帮我看看' },
      { id: 'a1', role: 'assistant', content: '已经做完了' },
    ],
    busy: false,
    event_seq: 42,
    event_epoch: 'e1',
  };
  const next = reconcileTranscriptWithServer(stuck, data, stuck, [], { serverBusy: false });
  assert.equal(next.busy, false);
  assert.equal(next.activity, null);
  assert.equal(next.activeTurnId, '');
  assert.equal(next.status, 'idle');
  assert.equal(next.lastSeq, 42);
  assert.equal(next.isLive, true);
  assert.equal(next.items.at(-1).content, '已经做完了');
  console.log('[pass] reconcile clears a stuck running state when the server is idle');
}

// 场景:会话已从 daemon 内存卸载,分页历史不带 busy 字段,只有探针说 busy=false。
// 期望:按探针结果视为空闲,运行态同样被清掉。
{
  const stuck = stuckState();
  const next = reconcileTranscriptWithServer(
    stuck, { messages: [{ id: 'u1', role: 'user', content: '帮我看看' }] }, stuck, [], { serverBusy: false },
  );
  assert.equal(next.busy, false);
  assert.equal(next.activity, null);
  assert.equal(next.lastSeq, stuck.lastSeq, 'without a server seq the old cursor is kept');
  console.log('[pass] reconcile trusts the probe when an unloaded session snapshot has no busy flag');
}

// 场景:服务端仍在忙(页面只是漏了事件),补拉事件环里有更新的进度。
// 期望:保持运行态,应用补拉的事件,lastSeq 前进到补拉的最新 seq。
{
  const stuck = stuckState();
  const next = reconcileTranscriptWithServer(
    stuck,
    { messages: [{ id: 'u1', role: 'user', content: '帮我看看' }], busy: true, active_turn_id: 't1' },
    stuck,
    [
      { type: 'busy_changed', seq: 2, payload: { busy: true, turn_id: 't1' } },
      { type: 'agent_progress', seq: 9, payload: { phase: 'tool_running', label: '正在运行命令' } },
    ],
    { serverBusy: true },
  );
  assert.equal(next.busy, true);
  assert.equal(next.activity.label, '正在运行命令');
  assert.equal(next.lastSeq, 9);
  console.log('[pass] reconcile keeps a running turn and applies the catch-up replay');
}

// ---- 生产 hook ----
const moduleUrl = new URL('./sessionTranscript.js', import.meta.url);
const source = readFileSync(moduleUrl, 'utf8');
const dependencies = {};
const imports = /^import \{([^}]+)\} from '([^']+)';$/gm;
for (const [, names, specifier] of source.matchAll(imports)) {
  if (specifier === 'react') continue;
  const module = await import(new URL(specifier, moduleUrl));
  for (const name of names.split(',').map((value) => value.trim())) dependencies[name] = module[name];
}
const body = source.replace(imports, '').replace(/^export /gm, '');

function harness() {
  const slots = [];
  const requests = [];
  const probes = [];
  const reports = [];
  const resubscribes = [];
  const listeners = new Set();
  const intervals = new Map();
  let nextInterval = 1;
  let cursor = 0;
  let effects = [];
  let now = 1_000_000;
  const FakeDate = class extends Date {
    static now() { return now; }
  };
  const changed = (previous, next) => !previous || next.some((value, i) => value !== previous[i]);
  const hooks = {
    useRef(initial) {
      const index = cursor++;
      return slots[index] ||= { current: initial };
    },
    useMemo(factory, deps) {
      const index = cursor++;
      if (changed(slots[index]?.deps, deps)) slots[index] = { deps, value: factory() };
      return slots[index].value;
    },
    useCallback(callback, deps) { return hooks.useMemo(() => callback, deps); },
    useSyncExternalStore(_subscribe, snapshot) { cursor++; return snapshot(); },
    useEffect(callback, deps) {
      const index = cursor++;
      if (changed(slots[index]?.deps, deps)) {
        const previous = slots[index];
        slots[index] = { deps };
        effects.push(() => { previous?.cleanup?.(); slots[index].cleanup = callback(); });
      }
    },
  };
  const hook = vm.runInNewContext(`${body}\nuseSessionTranscript;`, {
    ...dependencies, ...hooks, console, Map, Set, Date: FakeDate,
    window: {
      setInterval: (fn, ms) => { const id = nextInterval++; intervals.set(id, { fn, ms }); return id; },
      clearInterval: (id) => intervals.delete(id),
    },
    requestAnimationFrame: () => 1, cancelAnimationFrame() {},
    createApi: () => ({
      getMessages: (sid, since) => new Promise((resolve, reject) => requests.push({ sid, since, resolve, reject })),
      getSessionRuntime: (sid) => new Promise((resolve, reject) => probes.push({ sid, resolve, reject })),
      reportTranscriptStall: (report) => { reports.push(report); return Promise.resolve(); },
      reportSessionOpen: () => Promise.resolve(),
    }),
    connection: {
      reconfigure() {},
      addEventListener(_type, listener) { listeners.add(listener); },
      removeEventListener(_type, listener) { listeners.delete(listener); },
      retainSession() {}, releaseSession() {},
      resubscribe: (sid, options) => { resubscribes.push({ sid, ...options }); return true; },
      sessionCursor: () => ({ lastSeq: 0, epoch: 'e1' }),
      liveness: () => ({ wsState: 'open', msSinceMessage: 800, heartbeatTimeouts: 0 }),
    },
  });
  return {
    requests, probes, reports, resubscribes,
    advance(ms) { now += ms; },
    tick() {
      for (const { fn, ms } of intervals.values()) if (ms === dependencies.STALL_CHECK_TICK_MS) fn();
    },
    render(ref) {
      cursor = 0; effects = [];
      const result = hook(ref, { live: true });
      effects.forEach((effect) => effect());
      return result;
    },
    unmount() { slots.forEach((slot) => slot?.cleanup?.()); },
  };
}
const flush = async () => { for (let i = 0; i < 8; i++) await Promise.resolve(); };

// 场景:实时会话的回合进行到「正在推理」后事件流断了,daemon 其实已经收尾。
// 期望:20 秒内不探针;超过 20 秒后探针发现服务端空闲 → 上报 server_idle → 重拉历史尾页
// → 界面不再运行、显示最终回答 → 从服务端当前 seq 重新订阅并记下实例号。
{
  const h = harness();
  const ref = { sessionId: 'stuck', active: true };
  h.render(ref);
  h.requests[0].resolve({ messages: [{ id: 'u1', role: 'user', content: '帮我看看' }], busy: true, active_turn_id: 't1' });
  await flush();
  assert.equal(h.requests.length, 2, 'busy load requests the live catch-up');
  h.requests[1].resolve({ events: [
    { type: 'busy_changed', seq: 2, payload: { busy: true, turn_id: 't1' } },
    { type: 'agent_progress', seq: 3, payload: { phase: 'reasoning', label: '正在推理' } },
  ] });
  await flush();
  let view = h.render(ref);
  assert.equal(view.busy, true);
  assert.equal(view.activity.label, '正在推理');

  h.advance(10_000);
  h.tick();
  assert.equal(h.probes.length, 0, 'ten quiet seconds are normal and must not probe');

  h.advance(15_000);
  h.tick();
  assert.equal(h.probes.length, 1, 'twenty-five quiet seconds trigger one probe');
  h.probes[0].resolve({ session_id: 'stuck', active: true, busy: false, event_seq: 12, event_epoch: 'e1' });
  await flush();
  assert.equal(h.reports.length, 1);
  assert.equal(h.reports[0].reason, 'server_idle');
  assert.equal(h.reports[0].action, 'resync');
  const reload = h.requests.at(-1);
  assert.equal(reload.since.limit, 200);
  reload.resolve({
    messages: [
      { id: 'u1', role: 'user', content: '帮我看看' },
      { id: 'a1', role: 'assistant', content: '已经做完了' },
    ],
    busy: false,
    event_seq: 12,
    event_epoch: 'e1',
  });
  await flush();
  view = h.render(ref);
  assert.equal(view.busy, false, 'the stuck reasoning indicator is cleared');
  assert.equal(view.activity, null);
  assert.equal(view.items.at(-1).content, '已经做完了');
  assert.deepEqual(h.resubscribes, [{ sid: 'stuck', since: 12, epoch: 'e1' }]);
  h.unmount();
  console.log('[pass] a stalled live transcript reconciles with an idle server like switching sessions');
}

// 场景:回合在跑、很久没事件,但服务端仍忙且 seq 与页面一致(长时间无输出的工具)。
// 期望:只探针、不上报、不重拉历史、不重订阅,界面保持运行。
{
  const h = harness();
  const ref = { sessionId: 'quiet', active: true };
  h.render(ref);
  h.requests[0].resolve({ messages: [{ id: 'u1', role: 'user', content: '跑个构建' }], busy: true, active_turn_id: 't1' });
  await flush();
  h.requests[1].resolve({ events: [
    { type: 'busy_changed', seq: 2, payload: { busy: true, turn_id: 't1' } },
    { type: 'tool_start', seq: 3, payload: { tool: 'bash', tool_call_id: 'c1' } },
  ] });
  await flush();
  h.render(ref);
  h.advance(25_000);
  h.tick();
  assert.equal(h.probes.length, 1);
  h.probes[0].resolve({ session_id: 'quiet', active: true, busy: true, event_seq: 3, event_epoch: 'e1' });
  await flush();
  assert.equal(h.reports.length, 0);
  assert.equal(h.requests.length, 2, 'no history reload for a quiet but healthy turn');
  assert.equal(h.resubscribes.length, 0);
  assert.equal(h.render(ref).busy, true);
  h.unmount();
  console.log('[pass] a quiet but healthy turn is only probed, never rewritten');
}
