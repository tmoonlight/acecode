import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { createLastSessionRecorder, lastSessionRecordFromRef } from './lastSessionMemory.js';
import { openSessionTargetFromSearch, stripOpenSessionParams } from './sessionJump.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

async function testAsync(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 场景:用户停在某个工作区会话上。期望:记录带会话 id 与工作区 hash,
// 桌面壳据此拼出 ?open=<会话>&workspace=<hash>。
test('workspace session ref becomes a record with its workspace hash', () => {
  assert.deepEqual(
    lastSessionRecordFromRef({ sessionId: 's1', workspaceHash: 'w1', contextId: 'default' }),
    { session_id: 's1', workspace_hash: 'w1' },
  );
});

// 场景:无工作区会话 / 只读打开的 TUI 会话。期望:分别带 no_workspace、read_only,
// 且无工作区会话不携带 workspace_hash(否则启动时会按工作区去 resume 而 404)。
test('no-workspace and read-only sessions keep their flags', () => {
  assert.deepEqual(
    lastSessionRecordFromRef({ sessionId: 's2', noWorkspace: true, workspaceHash: 'stale' }),
    { session_id: 's2', no_workspace: true },
  );
  assert.deepEqual(
    lastSessionRecordFromRef({ sessionId: 's3', workspaceHash: 'w3', readOnly: true }),
    { session_id: 's3', workspace_hash: 'w3', read_only: true },
  );
});

// 场景:用户停在首页 / 定时任务页 / 专家组件页,或还没有任何页面。
// 期望:没有记录(上报 null),下次启动落在首页而不是更早的某个会话。
test('home, loop and expert pages are not sessions', () => {
  assert.equal(lastSessionRecordFromRef(null), null);
  assert.equal(lastSessionRecordFromRef({ home: true, workspaceHash: 'w1' }), null);
  assert.equal(lastSessionRecordFromRef({ loop: true }), null);
  assert.equal(lastSessionRecordFromRef({ expertComponents: true }), null);
  assert.equal(lastSessionRecordFromRef({ workspaceHash: 'w1' }), null);
});

// 场景:activeRef 因标题 / 模型更新而换了对象,但仍是同一个会话。
// 期望:只上报一次;切到别的会话、回到首页时各上报一次。
// 回归:每次 activeRef 换对象都写一遍 state.json。
test('recorder reports each distinct destination once', () => {
  const calls = [];
  const recorder = createLastSessionRecorder({
    aceDesktop_setLastSession: (record) => { calls.push(record); return '{"ok":true}'; },
  });
  assert.equal(recorder.remember({ sessionId: 's1', workspaceHash: 'w1' }), true);
  assert.equal(recorder.remember({ sessionId: 's1', workspaceHash: 'w1', title: 'renamed' }), false);
  assert.equal(recorder.remember({ sessionId: 's2', workspaceHash: 'w1' }), true);
  assert.equal(recorder.remember({ home: true, workspaceHash: 'w1' }), true);
  assert.equal(recorder.remember(null), false);
  assert.deepEqual(calls, [
    { session_id: 's1', workspace_hash: 'w1' },
    { session_id: 's2', workspace_hash: 'w1' },
    null,
  ]);
});

// 场景:浏览器直连 daemon(没有桌面壳的 bridge)。期望:什么都不做、不抛异常。
test('recorder is inert without the desktop bridge', () => {
  assert.equal(createLastSessionRecorder({}).remember({ sessionId: 's1' }), false);
  assert.equal(createLastSessionRecorder(undefined).remember({ sessionId: 's1' }), false);
});

// 场景:bridge 调用失败(桌面壳正在退出)。期望:不抛到渲染流程里,
// 且同一个会话下次还能重新上报,而不是被去重吞掉。
await testAsync('a failed report can be retried', async () => {
  let attempts = 0;
  const recorder = createLastSessionRecorder({
    aceDesktop_setLastSession: () => {
      attempts += 1;
      return attempts === 1 ? Promise.reject(new Error('shutting down')) : '{"ok":true}';
    },
  });
  assert.equal(recorder.remember({ sessionId: 's1' }), true);
  await new Promise((resolve) => setTimeout(resolve, 0));
  assert.equal(recorder.remember({ sessionId: 's1' }), true);
  assert.equal(attempts, 2);
});

// 场景:桌面壳自动恢复时在 URL 上带 restore=1。期望:落地目标标记 restored,
// 且该参数与 open 一起从地址栏抹掉(刷新不会二次触发)。
test('restore marker is parsed and stripped with the open params', () => {
  const target = openSessionTargetFromSearch('?token=t&open=s1&workspace=w1&restore=1');
  assert.equal(target.sessionId, 's1');
  assert.equal(target.workspaceHash, 'w1');
  assert.equal(target.restored, true);
  assert.equal(openSessionTargetFromSearch('?open=s1&workspace=w1').restored, undefined);
  assert.equal(stripOpenSessionParams('?token=t&open=s1&workspace=w1&restore=1'), 'token=t');
});

// 架构守护:App 只在启动跳转落定后上报(否则空的 activeRef 会清掉上次的记录),
// 自动恢复失败不弹错误;桌面壳要绑定 bridge 并在导航前拼上落地参数。
test('App and Desktop wire the last-session memory end to end', () => {
  const app = readFileSync(new URL('../App.jsx', import.meta.url), 'utf8');
  assert.match(
    app,
    /if \(authState !== 'ok' \|\| !startupNavigationSettled\) return;\s+lastSessionRecorderRef\.current\.remember\(activeRef\);/,
  );
  assert.match(app, /const silent = target\?\.restored === true;/);
  assert.equal(app.match(/navigationIsPending\(\) && !silent/g)?.length, 2);

  const desktop = readFileSync(
    new URL('../../../src/apps/desktop/main.cpp', import.meta.url), 'utf8');
  assert.match(desktop, /host\.bind\("aceDesktop_setLastSession", handle_set_last_session\);/);
  assert.match(
    desktop,
    /if \(restore_session\) url = last_session_url\(url, \*restore_session\);\s+host\.navigate\(url\);/,
  );
});
