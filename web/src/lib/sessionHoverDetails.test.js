import assert from 'node:assert/strict';
import {
  SESSION_HOVER_LIFECYCLE_ACTIONS,
  SESSION_HOVER_GIT_CACHE_TTL_MS,
  activeSessionHoverOwner,
  computeSessionHoverCardPosition,
  createSessionHoverLifecycleState,
  createSessionHoverGitInfoCache,
  reduceSessionHoverLifecycle,
  sessionHoverDetails,
  sessionHoverFocusIsVisible,
} from './sessionHoverDetails.js';

async function test(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await test('workspace session exposes cwd and confirmed Git branch', () => {
  assert.deepEqual(
    sessionHoverDetails(
      { cwd: 'C:\\repo', workspace_hash: 'workspace-a' },
      { is_repo: true, branch: 'feature/hover-details' },
    ),
    {
      cwd: 'C:\\repo',
      branch: 'feature/hover-details',
      isGitRepository: true,
    },
  );
});

await test('non-Git and failed lookup states keep directory-only details', () => {
  assert.deepEqual(
    sessionHoverDetails({ cwd: '/work/project' }, { is_repo: false }),
    { cwd: '/work/project', branch: '', isGitRepository: false },
  );
  assert.deepEqual(
    sessionHoverDetails({ cwd: '/work/project' }),
    { cwd: '/work/project', branch: '', isGitRepository: false },
  );
});

await test('no-workspace marker is authoritative even when malformed input has cwd', () => {
  assert.equal(sessionHoverDetails({ no_workspace: true, cwd: 'C:\\private' }), null);
  assert.equal(sessionHoverDetails({ noWorkspace: true, cwd: '/private' }), null);
  assert.equal(sessionHoverDetails({ cwd: '   ' }), null);
  assert.equal(sessionHoverDetails(null), null);
});

await test('session hover lifecycle keeps one active owner and ignores stale exits', () => {
  let state = createSessionHoverLifecycleState();
  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_ENTER,
    owner: 'row-a',
  });
  assert.equal(activeSessionHoverOwner(state), 'row-a');

  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_ENTER,
    owner: 'row-b',
  });
  assert.equal(activeSessionHoverOwner(state), 'row-b');

  const afterStaleLeave = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_LEAVE,
    owner: 'row-a',
  });
  assert.equal(afterStaleLeave, state);
  assert.equal(activeSessionHoverOwner(afterStaleLeave), 'row-b');
});

await test('pointer hover overrides keyboard owner and restores it on leave', () => {
  let state = createSessionHoverLifecycleState();
  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.KEYBOARD_ENTER,
    owner: 'keyboard-row',
  });
  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_ENTER,
    owner: 'pointer-row',
  });
  assert.deepEqual(state, {
    pointerOwner: 'pointer-row',
    keyboardOwner: 'keyboard-row',
  });
  assert.equal(activeSessionHoverOwner(state), 'pointer-row');

  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.POINTER_LEAVE,
    owner: 'pointer-row',
  });
  assert.equal(activeSessionHoverOwner(state), 'keyboard-row');

  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_OWNER,
    owner: 'keyboard-row',
  });
  assert.deepEqual(state, createSessionHoverLifecycleState());
});

await test('clear-all removes both owners and is idempotent when already empty', () => {
  let state = {
    pointerOwner: 'pointer-row',
    keyboardOwner: 'keyboard-row',
  };
  state = reduceSessionHoverLifecycle(state, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_ALL,
  });
  assert.deepEqual(state, createSessionHoverLifecycleState());
  assert.equal(
    reduceSessionHoverLifecycle(state, { type: SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_ALL }),
    state,
  );
});

await test('pointer modality clears keyboard ownership without hiding pointer hover', () => {
  const state = reduceSessionHoverLifecycle({
    pointerOwner: 'pointer-row',
    keyboardOwner: 'keyboard-row',
  }, {
    type: SESSION_HOVER_LIFECYCLE_ACTIONS.CLEAR_KEYBOARD,
  });
  assert.deepEqual(state, {
    pointerOwner: 'pointer-row',
    keyboardOwner: '',
  });
  assert.equal(activeSessionHoverOwner(state), 'pointer-row');
});

await test('focus intent excludes pointer focus and safely detects keyboard-visible focus', () => {
  const visibleTarget = { matches: (selector) => selector === ':focus-visible' };
  const hiddenTarget = { matches: () => false };
  const unsupportedTarget = { matches: () => { throw new SyntaxError('unsupported selector'); } };

  assert.equal(sessionHoverFocusIsVisible(visibleTarget), true);
  assert.equal(sessionHoverFocusIsVisible(hiddenTarget), false);
  assert.equal(sessionHoverFocusIsVisible(visibleTarget, { pointerInitiated: true }), false);
  assert.equal(sessionHoverFocusIsVisible({}, { pointerInitiated: false }), true);
  assert.equal(sessionHoverFocusIsVisible(unsupportedTarget), true);
});

await test('position prefers the right side and vertically centers when room exists', () => {
  assert.deepEqual(
    computeSessionHoverCardPosition({
      anchorRect: { left: 20, right: 220, top: 100, height: 30 },
      cardWidth: 320,
      cardHeight: 90,
      viewportWidth: 1200,
      viewportHeight: 800,
    }),
    { placement: 'right', left: 228, top: 70, maxHeight: 784 },
  );
});

await test('position flips left and clamps to viewport edges', () => {
  assert.deepEqual(
    computeSessionHoverCardPosition({
      anchorRect: { left: 800, right: 990, top: 760, height: 30 },
      cardWidth: 300,
      cardHeight: 120,
      viewportWidth: 1000,
      viewportHeight: 800,
    }),
    { placement: 'left', left: 492, top: 672, maxHeight: 784 },
  );
});

await test('Git cache deduplicates in-flight loads and expires by TTL', async () => {
  let timestamp = 1_000;
  let calls = 0;
  let resolveFirst;
  const cache = createSessionHoverGitInfoCache(
    async (cwd) => {
      calls += 1;
      if (calls === 1) {
        return new Promise((resolve) => { resolveFirst = () => resolve({ is_repo: true, branch: cwd }); });
      }
      return { is_repo: true, branch: `reload-${cwd}` };
    },
    { now: () => timestamp },
  );

  const first = cache.get('/repo');
  const duplicate = cache.get('/repo');
  assert.equal(first, duplicate);
  await Promise.resolve();
  resolveFirst();
  assert.equal((await first).branch, '/repo');
  assert.equal(calls, 1);

  assert.equal((await cache.get('/repo')).branch, '/repo');
  assert.equal(calls, 1);

  timestamp += SESSION_HOVER_GIT_CACHE_TTL_MS + 1;
  assert.equal((await cache.get('/repo')).branch, 'reload-/repo');
  assert.equal(calls, 2);
});

await test('Git cache invalidates one cwd without evicting another', async () => {
  let calls = 0;
  const cache = createSessionHoverGitInfoCache(async (cwd) => ({
    is_repo: true,
    branch: `${cwd}-${++calls}`,
  }));

  assert.equal((await cache.get('/a')).branch, '/a-1');
  assert.equal((await cache.get('/b')).branch, '/b-2');
  cache.invalidate('/a');
  assert.equal((await cache.get('/a')).branch, '/a-3');
  assert.equal((await cache.get('/b')).branch, '/b-2');
});

await test('Git cache does not retain failures and skips empty cwd', async () => {
  let calls = 0;
  const cache = createSessionHoverGitInfoCache(async () => {
    calls += 1;
    if (calls === 1) throw new Error('temporary failure');
    return { is_repo: false };
  });

  assert.equal(await cache.get(''), null);
  assert.equal(calls, 0);
  await assert.rejects(cache.get('/repo'), /temporary failure/);
  assert.deepEqual(await cache.get('/repo'), { is_repo: false });
  assert.equal(calls, 2);
});

console.log('sessionHoverDetails.test.js: all tests passed');
