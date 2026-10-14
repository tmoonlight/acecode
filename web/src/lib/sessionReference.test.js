import assert from 'node:assert/strict';
import {
  RECENT_SESSION_REFERENCE_LIMIT,
  eligibleSessionReferenceCandidates,
  extractSessionReferences,
  formatSessionReferenceToken,
  invalidateSessionReferenceData,
  loadSessionReferenceData,
  parseSessionReferenceToken,
  rankSessionReferenceCandidates,
  replaceQueryWithSessionReference,
  searchSessionReferenceData,
  SESSION_REFERENCE_QUERY_LIMIT,
  SESSION_REFERENCE_RECENT_FETCH_LIMIT,
  sessionReferenceTokenAt,
} from './sessionReference.js';

function test(name, fn) {
  try { fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

async function asyncTest(name, fn) {
  try { await fn(); console.log('ok -', name); }
  catch (error) { console.error('not ok -', name); throw error; }
}

const workspaceSession = {
  id: 'session-1',
  title: '修复 @ 菜单',
  workspace_hash: 'workspace-a',
  workspaceName: 'ACECode',
};

test('session token round-trips Unicode without whitespace', () => {
  const token = formatSessionReferenceToken(workspaceSession, { trailingSpace: false });
  assert.equal(/\s/.test(token), false);
  assert.deepEqual(parseSessionReferenceToken(token), {
    session_id: 'session-1',
    workspace_hash: 'workspace-a',
    no_workspace: false,
    title: '修复 @ 菜单',
    workspace_name: 'ACECode',
  });
  assert.deepEqual(sessionReferenceTokenAt(`先看 ${token} 后继续`, 3), {
    begin: 3,
    end: 3 + token.length,
    token,
    reference: parseSessionReferenceToken(token),
  });
});

test('query replacement inserts one stable token and restores the caret after it', () => {
  const replaced = replaceQueryWithSessionReference(
    '请参考 @修复 后继续',
    { begin: 4, end: 7 },
    workspaceSession,
  );
  const parsed = sessionReferenceTokenAt(replaced.text, 4);
  assert.ok(parsed);
  assert.equal(replaced.cursor, parsed.end + 1);
  assert.equal(replaced.text.slice(replaced.cursor), ' 后继续');
});

test('extraction humanizes markers and deduplicates structured references', () => {
  const token = formatSessionReferenceToken(workspaceSession, { trailingSpace: false });
  const result = extractSessionReferences(`比较 ${token} 和 ${token}`);
  assert.equal(result.displayText, '比较 @修复 @ 菜单 和 @修复 @ 菜单');
  assert.equal(result.references.length, 1);
  assert.equal(result.references[0].session_id, 'session-1');
});

test('eligibility excludes current, archived, child, and duplicate sessions', () => {
  const sessions = [
    workspaceSession,
    { ...workspaceSession },
    { id: 'current', title: 'current' },
    { id: 'archived', title: 'archived', archived: true },
    { id: 'child', title: 'child', parent_session_id: 'parent' },
    { id: 'task', title: 'free', no_workspace: true },
  ];
  const eligible = eligibleSessionReferenceCandidates(sessions, 'current', 'Task');
  assert.deepEqual(eligible.map((session) => session.id), ['session-1', 'task']);
  assert.equal(eligible[1].workspaceName, 'Task');
});

test('empty query is capped at recent ten while typed query returns every match', () => {
  const sessions = Array.from({ length: 14 }, (_, index) => ({
    id: `s-${index}`,
    title: `Needle ${index}`,
    workspace_hash: 'workspace-a',
    workspaceName: 'ACECode',
    updated_at: new Date(Date.UTC(2026, 0, index + 1)).toISOString(),
  }));
  const recent = rankSessionReferenceCandidates({
    sessions,
    query: '',
    now: Date.UTC(2026, 1, 1),
  });
  const matches = rankSessionReferenceCandidates({
    sessions,
    query: 'needle',
    now: Date.UTC(2026, 1, 1),
  });
  assert.equal(recent.length, RECENT_SESSION_REFERENCE_LIMIT);
  assert.equal(matches.length, sessions.length);
  assert.equal(matches[0].id, 's-13');
});

test('malformed and unsupported tokens are ignored', () => {
  assert.equal(parseSessionReferenceToken('@session:not-json'), null);
  const unsupported = `@session:${encodeURIComponent(JSON.stringify({ v: 2, i: 'x' }))}`;
  assert.equal(parseSessionReferenceToken(unsupported), null);
  assert.deepEqual(extractSessionReferences('plain @session:not-json text'), {
    displayText: 'plain @session:not-json text',
    references: [],
  });
});

await asyncTest('session list requests share a short-lived cache', async () => {
  let calls = 0;
  const api = {
    async listAllWorkspaceSessions() {
      calls += 1;
      return { sessions: [workspaceSession], workspaces: [], errors: [] };
    },
  };
  const [first, second] = await Promise.all([
    loadSessionReferenceData(api),
    loadSessionReferenceData(api),
  ]);
  assert.equal(calls, 1);
  assert.equal(first, second);
  invalidateSessionReferenceData(api);
  await loadSessionReferenceData(api);
  assert.equal(calls, 2);
});

// 构造一个只认服务端检索参数的假 api:记录每次 listGlobalSessions 的入参,
// 空查询只回「最近会话」,有查询词时按标题在「全部会话」里过滤 —— 与 daemon 的
// /api/session-search/sessions 行为一致(空查询最多 50 条,有查询时覆盖全部会话)。
function fakeGlobalSearchApi({ recent = [], all = [], progressSequence = null } = {}) {
  const calls = [];
  let round = 0;
  return {
    calls,
    async listGlobalSessions(options = {}) {
      calls.push({ ...options });
      const query = String(options.query || '');
      const progress = progressSequence
        ? progressSequence[Math.min(round, progressSequence.length - 1)]
        : { complete: true };
      round += 1;
      const sessions = query
        ? all.filter((session) => session.title.includes(query))
        : recent;
      return { sessions, errors: [], progress, next_cursor: null };
    },
    async listAllWorkspaceSessions() {
      throw new Error('@ 菜单不应再走 listAllWorkspaceSessions(会多请求一次 /api/workspaces)');
    },
  };
}

const oldSession = {
  id: '20260907-042156-0b2b',
  title: '随机选择题挑战',
  summary: '测试',
  workspace_hash: '68380109e8268da9',
  workspaceName: 'preserve-preview-ui-editing',
  updated_at: '2026-09-07T04:50:33Z',
};
const recentSessions = Array.from({ length: 50 }, (_, index) => ({
  id: `recent-${index}`,
  title: `最近的会话 ${index}`,
  workspace_hash: 'workspace-a',
  workspaceName: 'ACECode',
  updated_at: new Date(Date.UTC(2026, 9, 1, index)).toISOString(),
}));

// 场景:用户在 @ 后输入一个 36 天前会话的完整标题「随机选择题挑战」,而比它新的会话有上千个。
// 期望:有查询词时把查询词交给服务端全局目录过滤(带 query 与上限),旧会话能被搜到并排第一。
// 回归:旧实现只拉空查询的「最近 50 条」在本地过滤,目标会话不在其中,下拉框显示
// 「没有匹配的文件、文件夹或会话」。
await asyncTest('typed query searches the global catalog instead of filtering recent sessions', async () => {
  const api = fakeGlobalSearchApi({
    recent: recentSessions,
    all: [...recentSessions, oldSession],
  });
  const data = await searchSessionReferenceData(api, '随机选择题挑战');
  assert.deepEqual(api.calls.map((call) => ({ query: call.query, limit: call.limit })), [
    { query: '随机选择题挑战', limit: SESSION_REFERENCE_QUERY_LIMIT },
  ]);
  const ranked = rankSessionReferenceCandidates({
    sessions: data.sessions,
    query: '随机选择题挑战',
    now: Date.UTC(2026, 9, 14),
  });
  assert.equal(ranked[0]?.id, oldSession.id);
  assert.equal(ranked[0]?.workspaceName, 'preserve-preview-ui-editing');
});

// 场景:光标在同一个 @ 词里移动、或删掉一个字又敲回来,同一查询词在短时间内被重复检索。
// 期望:15 秒内同一查询词只请求一次;invalidate 后重新请求。每次按键都打一遍全量目录过滤没有必要。
await asyncTest('same typed query reuses a short-lived cache until invalidated', async () => {
  const api = fakeGlobalSearchApi({ recent: recentSessions, all: [oldSession] });
  await searchSessionReferenceData(api, '随机');
  await searchSessionReferenceData(api, '  随机  ');
  assert.equal(api.calls.length, 1);
  invalidateSessionReferenceData(api);
  await searchSessionReferenceData(api, '随机');
  assert.equal(api.calls.length, 2);
});

// 场景:daemon 刚启动,全局会话目录还在后台预热,服务端返回 progress.complete=false 的部分结果。
// 期望:先通过 onPartial 交出部分结果供下拉框立即显示,再按间隔重查直到目录完整;
// 不完整的结果不进缓存(否则目录预热完成后 15 秒内仍拿到残缺结果)。
await asyncTest('incomplete catalog is polled until complete and never cached', async () => {
  const api = fakeGlobalSearchApi({
    all: [oldSession],
    progressSequence: [{ complete: false }, { complete: false }, { complete: true }],
  });
  const partials = [];
  const data = await searchSessionReferenceData(api, '随机', {
    pollMs: 0,
    onPartial: (partial) => partials.push(partial),
  });
  assert.equal(api.calls.length, 3);
  assert.equal(partials.length, 2);
  assert.equal(data.progress.complete, true);
  assert.equal(data.sessions[0].id, oldSession.id);

  const capped = fakeGlobalSearchApi({ all: [oldSession], progressSequence: [{ complete: false }] });
  const cappedData = await searchSessionReferenceData(capped, '随机', { pollMs: 0, maxPolls: 3 });
  // 目录一直不完整时轮询有上限,返回最后一次的部分结果,而且不缓存。
  assert.equal(capped.calls.length, 3);
  assert.equal(cappedData.sessions[0].id, oldSession.id);
  await searchSessionReferenceData(capped, '随机', { pollMs: 0, maxPolls: 1 });
  assert.equal(capped.calls.length, 4);
});

// 场景:下拉框关闭或查询词改变,InputBar 中止上一轮检索。
// 期望:中止后不再发起新的轮询请求(停在等待间隔里的循环立刻退出)。
await asyncTest('aborting stops catalog polling', async () => {
  const api = fakeGlobalSearchApi({ all: [oldSession], progressSequence: [{ complete: false }] });
  const controller = new AbortController();
  const pending = searchSessionReferenceData(api, '随机', {
    signal: controller.signal,
    pollMs: 60_000,
    onPartial: () => controller.abort(),
  });
  await pending;
  assert.equal(api.calls.length, 1);
});

// 场景:只输入 @(空查询)。
// 期望:只向会话目录要最近 50 条,不再经 listAllWorkspaceSessions 顺带请求 /api/workspaces。
await asyncTest('empty query loads recent sessions from the catalog only', async () => {
  const api = fakeGlobalSearchApi({ recent: recentSessions });
  const data = await searchSessionReferenceData(api, '   ');
  assert.equal(api.calls.length, 1);
  assert.equal(api.calls[0].query, undefined);
  assert.equal(api.calls[0].limit, SESSION_REFERENCE_RECENT_FETCH_LIMIT);
  assert.equal(data.sessions.length, recentSessions.length);
});
