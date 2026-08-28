import assert from 'node:assert/strict';
import {
  desktopOpenSessionUrl,
  openSessionTargetFromSearch,
  sessionJumpMessageOrdinal,
  sessionJumpReadOnly,
  sessionJumpWorkspaceVisible,
  sessionRefFromJumpTarget,
  stripOpenSessionParams,
} from './sessionJump.js';
import { navigationHistoryFromHash } from './navigationHistory.js';
import { sessionWorkingCwd } from './previewTabs.js';

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('open session search params preserve workspace identity', () => {
  const target = openSessionTargetFromSearch('?token=t1&open=s1&workspace=w1');
  assert.deepEqual(target, {
    sessionId: 's1',
    workspaceHash: 'w1',
    noWorkspace: false,
    readOnly: false,
  });
  assert.equal(stripOpenSessionParams('?token=t1&open=s1&workspace=w1'), 'token=t1');
});

test('desktop open session URL carries workspace hash to landing page', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok+/=',
    sessionId: 's1',
    workspaceHash: 'w hash',
    protocol: 'http:',
  });
  assert.equal(url, 'http://127.0.0.1:4567/?token=tok%2B%2F%3D&open=s1&workspace=w+hash');
});

test('desktop open session URL can target no-workspace sessions', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's1',
    noWorkspace: true,
  });
  assert.equal(url, 'http://127.0.0.1:4567/?token=tok&open=s1&no_workspace=1');
});

test('desktop open session URL preserves explicit TUI-owned read-only mode', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's1',
    workspaceHash: 'w1',
    readOnly: true,
  });
  assert.equal(
    url,
    'http://127.0.0.1:4567/?token=tok&open=s1&workspace=w1&read_only=1',
  );

  const target = openSessionTargetFromSearch(
    '?token=tok&open=s1&workspace=w1&read_only=1',
  );
  assert.equal(sessionJumpReadOnly(target), true);
  assert.equal(stripOpenSessionParams(
    '?token=tok&open=s1&workspace=w1&read_only=1',
  ), 'token=tok');
  const ref = sessionRefFromJumpTarget(target);
  assert.equal(ref.readOnly, true);
  assert.equal(ref.externalSurface, 'tui');
});

test('session ref from jump target merges resume result and search metadata', () => {
  const ref = sessionRefFromJumpTarget(
    {
      id: 's1',
      workspace_hash: 'w-search',
      display_title: 'Search title',
      session_path: 'C:/Users/test/.acecode/projects/hash/s1.jsonl',
      message_count: 3,
      remote_control_bound: true,
      search_match: { kind: 'user_message', message_ordinal: 7, snippet: 'needle' },
    },
    {
      session_id: 's1',
      workspace_hash: 'w-resumed',
      cwd: 'N:/repo',
      active: true,
    },
  );
  assert.equal(ref.sessionId, 's1');
  assert.equal(ref.workspaceHash, 'w-resumed');
  assert.equal(ref.cwd, 'N:/repo');
  assert.equal(ref.displayTitle, 'Search title');
  assert.equal(ref.sessionPath, 'C:/Users/test/.acecode/projects/hash/s1.jsonl');
  assert.equal(ref.message_count, 3);
  assert.equal(ref.remote_control_bound, true);
  assert.equal(ref.contextId, 'default');
  assert.equal(ref.searchMatch.messageOrdinal, 7);
  assert.equal(ref.searchMatch.message_ordinal, 7);
  assert.equal(ref.searchMatch.snippet, 'needle');
});

test('hidden workspace search targets bypass Desktop activation and preserve visibility', () => {
  assert.equal(sessionJumpWorkspaceVisible({}), true);
  assert.equal(sessionJumpWorkspaceVisible({ workspace_visible: true }), true);
  assert.equal(sessionJumpWorkspaceVisible({ workspace_visible: false }), false);
  assert.equal(sessionJumpWorkspaceVisible({ workspaceVisible: 'false' }), false);

  const ref = sessionRefFromJumpTarget({
    id: 'hidden-session',
    workspace_hash: 'hidden-hash',
    workspace_visible: false,
  });
  assert.equal(ref.workspaceHash, 'hidden-hash');
  assert.equal(ref.workspace_visible, false);
  assert.equal(sessionJumpWorkspaceVisible(ref), false);
});

test('desktop open session URL preserves matched message ordinal', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's1',
    workspaceHash: 'w1',
    messageOrdinal: 12,
  });
  assert.equal(url, 'http://127.0.0.1:4567/?token=tok&open=s1&workspace=w1&message_ordinal=12');

  const target = openSessionTargetFromSearch('?open=s1&workspace=w1&message_ordinal=12');
  assert.equal(sessionJumpMessageOrdinal(target), 12);
  assert.deepEqual(target, {
    sessionId: 's1',
    workspaceHash: 'w1',
    noWorkspace: false,
    readOnly: false,
    search_match: { kind: 'user_message', message_ordinal: 12, messageOrdinal: 12 },
  });
  assert.equal(stripOpenSessionParams('?token=t1&open=s1&workspace=w1&message_ordinal=12'), 'token=t1');
});

test('desktop open session URL transfers navigation history in a client-only fragment', () => {
  const url = desktopOpenSessionUrl({
    port: 4567,
    token: 'tok',
    sessionId: 's2',
    workspaceHash: 'w2',
    navigationHistory: {
      back: [{
        workspaceHash: 'w1',
        sessionId: 's1',
        displayTitle: 'Previous',
        token: 'must-not-transfer',
      }],
      forward: [],
    },
  });
  const parsed = new URL(url);

  assert.equal(parsed.searchParams.get('open'), 's2');
  assert.equal(parsed.searchParams.has('ace_nav'), false);
  assert.doesNotMatch(parsed.hash, /must-not-transfer/);
  assert.deepEqual(navigationHistoryFromHash(parsed.hash), {
    back: [{
      workspaceHash: 'w1',
      sessionId: 's1',
      displayTitle: 'Previous',
    }],
    forward: [],
  });
});

// 触发场景：打开一个 no-workspace 会话（模型在 <data_dir>/cache/no-workspace/<id>
// 里生成了文件），用户点正文里的文件链接想预览。
// 期望行为：ref 仍然不带 workspace 归属（cwd 空、workspaceHash 空），
// 但携带 workingCwd —— 文件预览靠它定位目录。
// 回归背景：以前 ref 只有 cwd，而后端对 no-workspace 会话故意把 cwd 清成空串
// （它表达的是 workspace 归属，不是“文件在哪”），于是前端算出的预览根目录
// 恒为空，openFilePreview 第一行就 return，用户点自己刚生成的文件毫无反应。
test('no-workspace session ref carries a working directory for file preview', () => {
  const ref = sessionRefFromJumpTarget(
    { sessionId: 's1', noWorkspace: true },
    { session_id: 's1', no_workspace: true, cwd: '', working_cwd: 'C:\data\cache\no-workspace\s1' },
  );

  assert.equal(ref.noWorkspace, true);
  assert.equal(ref.workspaceHash, '');
  assert.equal(ref.cwd, '');
  assert.equal(ref.workingCwd, 'C:\data\cache\no-workspace\s1');
});

// 期望行为：普通 workspace 会话两个字段都有值且一致，
// workingCwd 只是把“工作目录”从 workspace 归属里拆出来，不改既有语义。
test('workspace session ref keeps cwd and workingCwd consistent', () => {
  const ref = sessionRefFromJumpTarget(
    { sessionId: 's2', workspaceHash: 'w1' },
    { session_id: 's2', cwd: 'N:/proj', working_cwd: 'N:/proj' },
  );

  assert.equal(ref.workspaceHash, 'w1');
  assert.equal(ref.cwd, 'N:/proj');
  assert.equal(ref.workingCwd, 'N:/proj');
});

// 触发场景：no-workspace 会话里模型生成了文件，用户点链接预览。
// 期望行为：预览根目录取会话自己的工作目录，而不是 daemon 进程的 cwd。
// 回归背景：ref.cwd 对 no-workspace 会话恒为空，旧逻辑会回退到 health.cwd
// （daemon 自己的工作目录）。实测中文件实际在
// C:/Users/shao/.acecode/cache/no-workspace/<id>/ 下，预览却去 N:/Users/shao/se/ 里找，
// 结果是一个看上去照模照样的「文件不存在」—— 比干脆打不开更难排查。
test('no-workspace preview root comes from the session, never the daemon cwd', () => {
  const ref = sessionRefFromJumpTarget(
    { sessionId: 's1', noWorkspace: true },
    { session_id: 's1', no_workspace: true, cwd: '', working_cwd: 'C:/data/cache/no-workspace/s1' },
  );

  // ChatView 算预览根目录的同一条优先级：workingCwd 赢，no-workspace 不取 daemon 兑底。
  const daemonCwd = 'N:/Users/shao/se';
  const previewRoot = sessionWorkingCwd({
    worktree: null,
    cwd: ref.workingCwd || ref.cwd || '',
    fallbackCwd: ref.noWorkspace ? '' : daemonCwd,
  });

  assert.equal(previewRoot, 'C:/data/cache/no-workspace/s1');
  assert.notEqual(previewRoot, daemonCwd);
});

// 期望行为：即使 working_cwd 缺失（连的是旧 daemon），no-workspace 会话也宁可算不出
// 根目录（预览不打开），也不能指向 daemon 的 cwd 去报一个错误的「文件不存在」。
test('no-workspace session without working_cwd yields no preview root at all', () => {
  const ref = sessionRefFromJumpTarget(
    { sessionId: 's1', noWorkspace: true },
    { session_id: 's1', no_workspace: true, cwd: '' },
  );

  const previewRoot = sessionWorkingCwd({
    worktree: null,
    cwd: ref.workingCwd || ref.cwd || '',
    fallbackCwd: ref.noWorkspace ? '' : 'N:/Users/shao/se',
  });

  assert.equal(previewRoot, '');
});
