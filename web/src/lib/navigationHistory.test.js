import assert from 'node:assert/strict';
import {
  deserializeNavigationHistory,
  goBack,
  goForward,
  navigationHistoryFromHash,
  navigationHistoryHash,
  navigationKey,
  pushNavigation,
  sameNavigationRef,
  serializeNavigationHistory,
  stripNavigationHistoryHash,
} from './navigationHistory.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('navigationKey distinguishes home and sessions', () => {
  assert.notEqual(
    navigationKey({ home: true, workspaceHash: 'w1', cwd: '/repo' }),
    navigationKey({ workspaceHash: 'w1', sessionId: 's1', cwd: '/repo' }),
  );
  assert.equal(
    navigationKey({ workspace_hash: 'w1', session_id: 's1', cwd: '/repo' }),
    navigationKey({ workspaceHash: 'w1', sessionId: 's1', cwd: '/repo' }),
  );
});

run('LOOP is a first-class navigation destination', () => {
  const session = { workspaceHash: 'w1', sessionId: 's1' };
  const loop = { loop: true };
  assert.notEqual(navigationKey(loop), navigationKey(session));
  const opened = pushNavigation({ back: [], forward: [] }, session, loop);
  const back = goBack(opened, loop);
  assert.deepEqual(back.activeRef, session);
  assert.deepEqual(goForward(back.history, session).activeRef, loop);
});

run('sameNavigationRef ignores display-only fields', () => {
  assert.equal(
    sameNavigationRef(
      { workspaceHash: 'w1', sessionId: 's1', displayTitle: 'Old' },
      { workspaceHash: 'w1', sessionId: 's1', displayTitle: 'New' },
    ),
    true,
  );
});

run('pushNavigation records current ref and clears forward stack', () => {
  const current = { workspaceHash: 'w1', sessionId: 'a' };
  const next = { workspaceHash: 'w1', sessionId: 'b' };
  const history = pushNavigation({ back: [], forward: [{ sessionId: 'future' }] }, current, next);
  assert.deepEqual(history.back, [current]);
  assert.deepEqual(history.forward, []);
});

run('goBack and goForward traverse active refs', () => {
  const a = { workspaceHash: 'w1', sessionId: 'a' };
  const b = { workspaceHash: 'w1', sessionId: 'b' };
  const c = { workspaceHash: 'w1', sessionId: 'c' };
  const history = { back: [a, b], forward: [] };

  const backResult = goBack(history, c);
  assert.deepEqual(backResult.activeRef, b);
  assert.deepEqual(backResult.history.back, [a]);
  assert.deepEqual(backResult.history.forward, [c]);

  const forwardResult = goForward(backResult.history, b);
  assert.deepEqual(forwardResult.activeRef, c);
  assert.deepEqual(forwardResult.history.back, [a, b]);
  assert.deepEqual(forwardResult.history.forward, []);
});

run('transfer snapshot round-trips bounded navigation fields without credentials', () => {
  const serialized = serializeNavigationHistory({
    back: [{
      workspace_hash: 'w1',
      session_id: 's1',
      context_id: 'ctx',
      cwd: 'N:/repo',
      display_title: 'First',
      read_only: true,
      token: 'secret-token',
      port: 4567,
      summary: 'large transcript-derived text',
      search_match: {
        message_ordinal: 7,
        snippet: 'private matching text',
      },
    }],
    forward: [{
      home: true,
      workspaceHash: 'w2',
      cwd: 'N:/other',
      workspaceName: 'Other',
    }],
  });

  assert.ok(serialized);
  assert.doesNotMatch(serialized, /secret-token|large transcript-derived|private matching text/);
  assert.deepEqual(deserializeNavigationHistory(serialized), {
    back: [{
      workspaceHash: 'w1',
      sessionId: 's1',
      contextId: 'ctx',
      cwd: 'N:/repo',
      displayTitle: 'First',
      readOnly: true,
      searchMatch: {
        kind: 'user_message',
        messageOrdinal: 7,
        message_ordinal: 7,
      },
    }],
    forward: [{
      workspaceHash: 'w2',
      cwd: 'N:/other',
      workspaceName: 'Other',
      home: true,
    }],
  });
});

run('transfer snapshot rejects malformed payloads and preserves the 80-entry bound', () => {
  assert.equal(deserializeNavigationHistory('not-json'), null);
  assert.equal(deserializeNavigationHistory(JSON.stringify({ v: 999, b: [], f: [] })), null);
  assert.equal(deserializeNavigationHistory('x'.repeat((64 * 1024) + 1)), null);

  const back = Array.from({ length: 100 }, (_, index) => ({
    workspaceHash: 'w1',
    sessionId: `s${index}`,
  }));
  const restored = deserializeNavigationHistory(serializeNavigationHistory({ back, forward: [] }));
  assert.equal(restored.back.length, 80);
  assert.equal(restored.back[0].sessionId, 's20');
  assert.equal(restored.back[79].sessionId, 's99');
});

run('navigation history fragment is one-shot and preserves unrelated hash parameters', () => {
  const history = {
    back: [{ workspaceHash: 'w1', sessionId: 's1' }],
    forward: [{ workspaceHash: 'w2', sessionId: 's2' }],
  };
  const hash = navigationHistoryHash(history);
  assert.match(hash, /^ace_nav=/);
  assert.deepEqual(navigationHistoryFromHash(`#panel=chat&${hash}`), history);
  assert.equal(stripNavigationHistoryHash(`#panel=chat&${hash}`), 'panel=chat');
  assert.equal(stripNavigationHistoryHash('#plain-anchor'), 'plain-anchor');
});
