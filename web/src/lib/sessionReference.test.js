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
