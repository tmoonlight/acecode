// mergeAllWorkspaceSessions 单元测试。
//
// 该函数是 SearchPalette 的数据入口:跨所有 workspace 拉 sessions、注入 workspace
// 名 / hash、单 workspace 失败不阻塞其它 workspace。这里只测纯合并/错误收集逻辑,
// 不打真实网络,通过依赖注入 listWorkspaces / listSessions 两个 mock 完成。

import assert from 'node:assert/strict';
import {
  ApiError,
  createApi,
  DEFAULT_REQUEST_TIMEOUT_MS,
  expertCapabilitiesPath,
  mergeAllWorkspaceSessions,
  mergeGlobalSessionsAndWorkspaces,
  PASTED_TEXT_UPLOAD_TIMEOUT_MS,
  sessionDraftPath,
  workspaceDraftAttachmentBlobPath,
  sessionTrajectoryPath,
  sessionTodosPath,
} from './api.js';

await run('archive aggregation scopes its compatibility request to no-workspace metadata', async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url) => {
    const parsed = new URL(url);
    calls.push(parsed);
    const body = parsed.pathname === '/api/workspaces' ? [{ hash: 'workspace', name: 'Project' }]
      : parsed.pathname === '/api/sessions' ? [{ id: 'task', no_workspace: true }]
        : [{ id: 'session' }];
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => body };
  };
  try {
    const api = createApi({ origin: 'http://archive.test', token: 'fixture' });
    const result = await api.listAllArchivedSessions();
    assert.equal(calls.length, 3);
    const compatibility = calls.find(url => url.pathname === '/api/sessions');
    assert.equal(compatibility.searchParams.get('scope'), 'no-workspace');
    assert.equal(compatibility.searchParams.get('archived'), '1');
    assert.equal(calls.find(url => url.pathname.endsWith('/workspace/sessions')).searchParams.get('archived'), '1');
    assert.deepEqual(result.sessions.map(row => row.id), ['session', 'task']);
    assert.equal(result.sessions[0].workspace_hash, 'workspace');
    assert.equal(result.sessions[1].no_workspace, true);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

await run('sidebar queries retain expansion, pagination and reveal when URLSearchParams.size is unavailable', async () => {
  const originalFetch = globalThis.fetch;
  const OriginalSearchParams = globalThis.URLSearchParams;
  const calls = [];
  globalThis.URLSearchParams = class extends OriginalSearchParams {
    get size() { return undefined; }
  };
  globalThis.fetch = async (url, options) => {
    calls.push({ url: new URL(url), options });
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ sessions: [] }) };
  };
  try {
    const api = createApi({ origin: 'http://sidebar.test', token: 'fixture-token' });
    const options = { limit: 5, expanded: ['loop:每日 / + 简报', 'session:older'], reveal: 'older / + 会话' };
    await api.listSidebarSessions('工作区 / one', options);
    await api.listSidebarSessions('', { ...options, limit: 0 });
    const move = { source: 'session:older', target: 'loop:每日 / + 简报', placement: 'inside' };
    await api.moveSidebarSession('工作区 / one', move, options);
    await api.listSidebarSessions('', { limit: 0, expanded: [], reveal: '' });
    assert.equal(calls[0].url.pathname, '/api/workspaces/' + encodeURIComponent('工作区 / one') + '/session-sidebar');
    assert.equal(calls[1].url.pathname, '/api/session-sidebar');
    for (const index of [0, 1, 2]) {
      assert.deepEqual(JSON.parse(calls[index].url.searchParams.get('expanded')), options.expanded);
      assert.equal(calls[index].url.searchParams.get('reveal'), options.reveal);
      assert.equal(calls[index].url.searchParams.get('limit'), index === 1 ? null : '5');
    }
    assert.equal(calls[2].options.method, 'POST');
    assert.deepEqual(JSON.parse(calls[2].options.body), move);
    assert.equal(calls[3].url.search, '');
  } finally {
    globalThis.fetch = originalFetch;
    globalThis.URLSearchParams = OriginalSearchParams;
  }
});

await run('local import sends original binary ZIP with authentication and preview digest', async () => {
  const originalFetch = globalThis.fetch;
  try {
    const calls = [];
    globalThis.fetch = async (url, options) => { calls.push({ url, options }); return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({}) }; };
    const api = createApi({ origin: 'http://127.0.0.1:49000', token: 'local-test-token' });
    const blob = new Blob(['zip-bytes'], { type: 'application/zip' });
    await api.previewThemeImport(blob);
    await api.importTheme(blob, 'a'.repeat(64));
    assert.equal(calls[0].url, 'http://127.0.0.1:49000/api/themes/import/preview');
    assert.equal(calls[0].options.body, blob);
    assert.equal(calls[1].options.body, blob);
    assert.equal(calls[1].options.headers['Content-Type'], 'application/zip');
    assert.equal(calls[1].options.headers['X-ACECode-Token'], 'local-test-token');
    assert.ok(calls[1].url.endsWith(`?sha256=${'a'.repeat(64)}`));
  } finally { globalThis.fetch = originalFetch; }
});

await run('suggestion HTTP helpers encode both IDs, authenticate and use explicit acceptance locations', async () => {
  const original = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({}) };
  };
  try {
    const api = createApi({ origin: 'http://suggestion.test', token: 'test-token' });
    await api.listTaskSuggestions('source /');
    await api.acceptTaskSuggestion('source /', 'suggestion /', 'current_branch');
    await api.dismissTaskSuggestion('source /', 'suggestion /');
    assert.equal(calls[0].url, 'http://suggestion.test/api/sessions/source%20%2F/suggestions');
    assert.equal(calls[1].url, 'http://suggestion.test/api/sessions/source%20%2F/suggestions/suggestion%20%2F/accept');
    assert.equal(calls[1].options.headers['X-ACECode-Token'], 'test-token');
    assert.deepEqual(JSON.parse(calls[1].options.body), { location: 'current_branch' });
    assert.ok(calls[2].url.endsWith('/dismiss'));
  } finally { globalThis.fetch = original; }
});

function run(name, fn) {
  try {
    const ret = fn();
    if (ret && typeof ret.then === 'function') {
      return ret.then(
        () => console.log(`[pass] ${name}`),
        (err) => { console.error(`[fail] ${name}`); throw err; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('expert capability catalog path carries only the workspace selector', () => {
  assert.equal(expertCapabilitiesPath(), '/api/experts/capabilities');
  assert.equal(
    expertCapabilitiesPath('项目 hash'),
    '/api/experts/capabilities?workspace=%E9%A1%B9%E7%9B%AE%20hash',
  );
});

await run('expert capability client reads the sanitized runtime catalog endpoint', async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ skills: [], mcp_servers: [], tools: [] }),
    };
  };
  try {
    const result = await createApi({ origin: 'http://acecode.test', token: '' })
      .listExpertCapabilities('workspace-a');
    assert.deepEqual(result, { skills: [], mcp_servers: [], tools: [] });
    assert.equal(calls[0].url, 'http://acecode.test/api/experts/capabilities?workspace=workspace-a');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.body, undefined);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

await run('Desktop source reference creation sends metadata without file bytes', async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 201,
      headers: { get: () => 'application/json' },
      json: async () => ({ attachment: { id: 'att_reference' } }),
    };
  };
  try {
    const body = {
      name: 'large.pdf',
      mime_type: 'application/pdf',
      source_path: 'C:/docs/large.pdf',
      reference_only: true,
    };
    await createApi({ origin: 'http://acecode.test', token: '' })
      .createSessionAttachmentReference('session/a', body);

    assert.equal(calls[0].url, 'http://acecode.test/api/sessions/session%2Fa/attachments');
    assert.equal(calls[0].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[0].opts.body), body);
    assert.equal('data_base64' in JSON.parse(calls[0].opts.body), false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

// 触发场景:粘贴的文本块走三条新接口(工作区草稿附件上传、导入到会话、读取附件正文)。
// 期望:路径正确、scope 为空时映射到 __no_workspace__;上传 / 读取默认 5 分钟超时,会话上传透传
// 调用方给的 timeoutMs(默认仍 30 秒);读取把响应当纯文本,即使 Content-Type 是 JSON 也不解析。
await run('pasted text attachment endpoints use workspace draft paths and paste timeouts', async () => {
  const originalFetch = globalThis.fetch;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  const calls = [];
  const delays = [];
  globalThis.setTimeout = (_callback, delay) => { delays.push(delay); return delays.length; };
  globalThis.clearTimeout = () => {};
  globalThis.fetch = async (url, opts) => {
    calls.push({ url, opts });
    const json = !url.endsWith('/blob');
    return {
      ok: true,
      status: 201,
      headers: { get: () => (json ? 'application/json' : 'application/json; charset=utf-8') },
      json: async () => ({ attachment: { id: 'x' } }),
      text: async () => '{"looks":"like json"}',
    };
  };
  try {
    const api = createApi({ origin: 'http://acecode.test', token: 'tok' });
    const body = { name: 'p.txt', mime_type: 'text/plain', data_base64: 'eA==', origin: 'pasted_text', paste: { chars: 1, lines: 1 } };
    await api.uploadWorkspaceDraftAttachment('', body);
    await api.uploadWorkspaceDraftAttachment('hash/1', body);
    await api.importWorkspaceDraftAttachment('sess 1', { workspace: '__no_workspace__', id: 'd1' });
    await api.uploadSessionAttachment('sess', body, { timeoutMs: PASTED_TEXT_UPLOAD_TIMEOUT_MS });
    await api.uploadSessionAttachment('sess', body);
    const text = await api.readAttachmentText('/api/workspaces/__no_workspace__/draft/attachments/d1/blob');

    assert.equal(calls[0].url, 'http://acecode.test/api/workspaces/__no_workspace__/draft/attachments');
    assert.equal(calls[1].url, 'http://acecode.test/api/workspaces/hash%2F1/draft/attachments');
    assert.deepEqual(JSON.parse(calls[0].opts.body), body);
    assert.equal(calls[2].url, 'http://acecode.test/api/sessions/sess%201/attachments');
    assert.deepEqual(JSON.parse(calls[2].opts.body), { from_workspace_draft: { workspace: '__no_workspace__', id: 'd1' } });
    assert.equal(calls[5].url, 'http://acecode.test/api/workspaces/__no_workspace__/draft/attachments/d1/blob');
    assert.equal(calls[5].opts.method, 'GET');
    assert.equal(calls[5].opts.headers['X-ACECode-Token'], 'tok', 'remote Web needs the token header, not a bare URL');
    assert.equal(text, '{"looks":"like json"}');
    assert.deepEqual(delays, [
      PASTED_TEXT_UPLOAD_TIMEOUT_MS, PASTED_TEXT_UPLOAD_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS,
      PASTED_TEXT_UPLOAD_TIMEOUT_MS, DEFAULT_REQUEST_TIMEOUT_MS, PASTED_TEXT_UPLOAD_TIMEOUT_MS,
    ]);
    assert.equal(workspaceDraftAttachmentBlobPath('', 'a/b'), '/api/workspaces/__no_workspace__/draft/attachments/a%2Fb/blob');
  } finally {
    globalThis.fetch = originalFetch;
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
});

await run('ordinary API requests abort with a structured timeout error', async () => {
  const originalFetch = globalThis.fetch;
  const originalSetTimeout = globalThis.setTimeout;
  const originalClearTimeout = globalThis.clearTimeout;
  let scheduledDelay = null;
  let clearedTimer = null;
  globalThis.setTimeout = (callback, delay) => {
    scheduledDelay = delay;
    queueMicrotask(callback);
    return 91;
  };
  globalThis.clearTimeout = (timer) => {
    clearedTimer = timer;
  };
  globalThis.fetch = async (_url, opts) => new Promise((_resolve, reject) => {
    opts.signal.addEventListener('abort', () => {
      const error = new Error('aborted');
      error.name = 'AbortError';
      reject(error);
    }, { once: true });
  });
  try {
    await assert.rejects(
      createApi({ origin: 'http://acecode.test', token: '' }).health(),
      (error) => error instanceof ApiError
        && error.status === 408
        && error.code === 'TIMEOUT',
    );
    assert.equal(scheduledDelay, DEFAULT_REQUEST_TIMEOUT_MS);
    assert.equal(clearedTimer, 91);
  } finally {
    globalThis.fetch = originalFetch;
    globalThis.setTimeout = originalSetTimeout;
    globalThis.clearTimeout = originalClearTimeout;
  }
});

await run('native-dialog API requests remain exempt from the default timeout', async () => {
  const originalFetch = globalThis.fetch;
  const originalSetTimeout = globalThis.setTimeout;
  let timerScheduled = false;
  globalThis.setTimeout = () => {
    timerScheduled = true;
    return 92;
  };
  globalThis.fetch = async (_url, opts) => ({
    ok: true,
    status: 200,
    headers: { get: () => 'application/json' },
    json: async () => ({ path: 'C:/workspace' }),
    signal: opts.signal,
  });
  try {
    await createApi({ origin: 'http://acecode.test', token: '' }).pickWorkspaceFolder();
    assert.equal(timerScheduled, false);
  } finally {
    globalThis.fetch = originalFetch;
    globalThis.setTimeout = originalSetTimeout;
  }
});

await run('expert switch client keeps optional draft text in the atomic request body', async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({
        expert: { id: 'reviewer', display_name: '代码审查' },
        receipt: { sequence: 7, expert_id: 'reviewer', state: 'applied', applied: true },
      }),
    };
  };
  try {
    const client = createApi({ origin: 'http://acecode.test', token: '' });
    await client.setSessionExpert('session/a', 'reviewer');
    await client.setSessionExpert('session/a', 'reviewer', { draftText: '' });
    await client.setSessionExpert('session/a', 'reviewer', { draftText: '审查当前改动' });
    await client.clearSessionExpert('session/a');

    assert.equal(calls[0].url, 'http://acecode.test/api/sessions/session%2Fa/expert');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { expert_id: 'reviewer' });
    assert.deepEqual(JSON.parse(calls[1].opts.body), { expert_id: 'reviewer', draft_text: '' });
    assert.deepEqual(JSON.parse(calls[2].opts.body), {
      expert_id: 'reviewer',
      draft_text: '审查当前改动',
    });
    assert.equal(calls[3].url, 'http://acecode.test/api/sessions/session%2Fa/expert');
    assert.equal(calls[3].opts.method, 'DELETE');
    assert.equal(calls[3].opts.body, undefined);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

await run('external AbortSignal stays a silent caller cancellation', async () => {
  const originalFetch = globalThis.fetch;
  globalThis.fetch = async (_url, opts) => new Promise((_resolve, reject) => {
    opts.signal.addEventListener('abort', () => {
      const error = new Error('caller cancelled');
      error.name = 'AbortError';
      reject(error);
    }, { once: true });
  });
  try {
    const controller = new AbortController();
    const pending = createApi({ origin: 'http://acecode.test', token: '' })
      .listGlobalSessions({ requestId: 'request-1', signal: controller.signal });
    controller.abort();
    await assert.rejects(pending, (error) => (
      error?.name === 'AbortError' && !(error instanceof ApiError)
    ));
  } finally {
    globalThis.fetch = originalFetch;
  }
});

await run('全局会话目录与可见项目并行合并且保留局部错误', async () => {
  const calls = [];
  const hidden = {
    id: 'hidden-session',
    workspace_hash: 'hidden-hash',
    workspace_visible: false,
  };
  const result = await mergeGlobalSessionsAndWorkspaces({
    listGlobalSessions: async () => {
      calls.push('catalog');
      await Promise.resolve();
      return {
        sessions: [hidden],
        errors: [{ hash: 'bad-hash', stage: 'metadata', message: 'bad project' }],
      };
    },
    listWorkspaces: async () => {
      calls.push('workspaces');
      return [{ hash: 'visible-hash', name: 'Visible' }];
    },
  });

  assert.deepEqual(calls, ['catalog', 'workspaces']);
  assert.deepEqual(result.sessions, [hidden]);
  assert.deepEqual(result.workspaces, [{ hash: 'visible-hash', name: 'Visible' }]);
  assert.equal(result.errors.length, 1);
  assert.equal(result.errors[0].hash, 'bad-hash');
  assert.equal(result.errors[0].stage, 'metadata');
});

await run('全局目录或项目列表失败时保留另一侧数据', async () => {
  const catalogFailed = await mergeGlobalSessionsAndWorkspaces({
    listGlobalSessions: () => { throw new Error('catalog unavailable'); },
    listWorkspaces: async () => [{ hash: 'visible' }],
  });
  assert.deepEqual(catalogFailed.sessions, []);
  assert.deepEqual(catalogFailed.workspaces, [{ hash: 'visible' }]);
  assert.match(catalogFailed.errors[0].message, /catalog unavailable/);

  const workspaceFailed = await mergeGlobalSessionsAndWorkspaces({
    listGlobalSessions: async () => ({ sessions: [{ id: 'still-searchable' }] }),
    listWorkspaces: async () => { throw new Error('projects unavailable'); },
  });
  assert.deepEqual(workspaceFailed.sessions, [{ id: 'still-searchable' }]);
  assert.deepEqual(workspaceFailed.workspaces, []);
  assert.equal(workspaceFailed.errors[0].name, 'workspace list');
  assert.match(workspaceFailed.errors[0].message, /projects unavailable/);
});

await run('listAllWorkspaceSessions 请求全局目录而非逐工作区会话', async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts) => {
    calls.push({ url, opts });
    const payload = url.endsWith('/api/session-search/sessions')
      ? { sessions: [{ id: 'hidden', workspace_visible: false }], errors: [] }
      : [{ hash: 'visible' }];
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => payload,
    };
  };
  try {
    const result = await createApi({ origin: 'http://acecode.test', token: 'tok' })
      .listAllWorkspaceSessions();
    assert.deepEqual(result.sessions, [{ id: 'hidden', workspace_visible: false }]);
    assert.deepEqual(result.workspaces, [{ hash: 'visible' }]);
    assert.deepEqual(calls.map((call) => call.url), [
      'http://acecode.test/api/session-search/sessions',
      'http://acecode.test/api/workspaces',
    ]);
    assert.equal(calls.some((call) => /\/api\/workspaces\/[^/]+\/sessions/.test(call.url)), false);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

await run('合并多个 workspace 的 sessions 并注入 workspaceName', async () => {
  const ws = [
    { hash: 'h1', name: 'acecode', cwd: '/acecode' },
    { hash: 'h2', name: 'hermes',  cwd: '/hermes' },
  ];
  const sessionsByHash = {
    h1: [
      { id: 's1', title: 'Hello',  updated_at: '2026-05-01' },
      { id: 's2', title: 'World',  updated_at: '2026-05-02' },
    ],
    h2: [
      { id: 's3', title: 'Bonjour', updated_at: '2026-05-03', workspace_hash: 'h2' },
    ],
  };
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions:   async (hash) => sessionsByHash[hash],
  });
  assert.equal(result.sessions.length, 3);
  assert.equal(result.errors.length, 0);
  assert.equal(result.sessions[0].workspaceName, 'acecode');
  assert.equal(result.sessions[0].workspace_hash, 'h1');
  assert.equal(result.sessions[2].workspaceName, 'hermes');
  assert.deepEqual(result.workspaces, ws);
  // 已有 workspace_hash 字段不被覆盖
  assert.equal(result.sessions[2].workspace_hash, 'h2');
});

await run('补入 no_workspace 活跃会话并过滤兼容列表中的普通会话', async () => {
  const ws = [{ hash: 'h1', name: 'acecode', cwd: '/acecode' }];
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions: async () => [{ id: 's1', title: 'Workspace task' }],
    listNoWorkspaceSessions: async () => [
      { id: 's1', title: 'Compatibility duplicate', workspace_hash: 'h1' },
      { id: 'temp-1', no_workspace: true, active: true },
    ],
  });

  assert.deepEqual(result.sessions.map((session) => session.id), ['s1', 'temp-1']);
  assert.deepEqual(result.workspaces, ws);
  const temporary = result.sessions[1];
  assert.equal(temporary.no_workspace, true);
  assert.equal(temporary.workspace_hash, '');
  assert.equal(temporary.workspaceName, '无工作区');
  assert.equal(temporary.cwd, '');
});

await run('no_workspace 补充请求失败不丢弃 workspace 结果', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => [{ hash: 'h1', name: 'acecode', cwd: '/acecode' }],
    listSessions: async () => [{ id: 's1', title: 'Still visible' }],
    listNoWorkspaceSessions: async () => { throw new Error('temporary list failed'); },
  });

  assert.deepEqual(result.sessions.map((session) => session.id), ['s1']);
  assert.equal(result.errors.length, 1);
  assert.equal(result.errors[0].name, '无工作区');
  assert.match(result.errors[0].message, /temporary list failed/);
});

await run('单个 workspace 拉取失败,其它 workspace 正常返回', async () => {
  const ws = [
    { hash: 'h1', name: 'good', cwd: '/good' },
    { hash: 'h2', name: 'bad',  cwd: '/bad' },
    { hash: 'h3', name: 'also-good', cwd: '/og' },
  ];
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions:   async (hash) => {
      if (hash === 'h2') throw new Error('500 oops');
      return [{ id: 's-' + hash, title: 't' }];
    },
  });
  assert.equal(result.sessions.length, 2);
  assert.equal(result.errors.length, 1);
  assert.equal(result.errors[0].hash, 'h2');
  assert.equal(result.errors[0].name, 'bad');
  assert.match(result.errors[0].message, /500 oops/);
});

await run('listWorkspaces 失败时返回空数组并把错误归档', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => { throw new Error('auth fail'); },
    listSessions:   async () => { throw new Error('should not call'); },
  });
  assert.deepEqual(result.sessions, []);
  assert.equal(result.errors.length, 1);
  assert.match(result.errors[0].message, /auth fail/);
});

await run('空 workspace 列表返回 empty result', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => [],
    listSessions:   async () => { throw new Error('should not call'); },
  });
  assert.deepEqual(result.sessions, []);
  assert.deepEqual(result.errors, []);
});

await run('listWorkspaces 返回非数组 → 视为空 + 不报错', async () => {
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => null,
    listSessions:   async () => [],
  });
  assert.deepEqual(result.sessions, []);
  assert.deepEqual(result.errors, []);
});

await run('listSessions 返回非数组 → 视为空 + 不报错', async () => {
  const ws = [{ hash: 'h1', name: 'x' }];
  const result = await mergeAllWorkspaceSessions({
    listWorkspaces: async () => ws,
    listSessions:   async () => null,
  });
  assert.equal(result.sessions.length, 0);
  assert.equal(result.errors.length, 0);
});

await run('getUsageStats uses usage endpoint query params and auth token', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ summary: { records: 0 } }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getUsageStats({ days: 7, workspace: 'w/a', timezoneOffsetMinutes: -480 });

    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/usage?days=7&workspace=w%2Fa&timezone_offset_minutes=-480');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('searchSessionUserMessages uses bounded content search endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ matches: [{ id: 's1' }] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.searchSessionUserMessages('sqlite 索引', 200);

    assert.deepEqual(result, { matches: [{ id: 's1' }] });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/session-search/user-messages?q=sqlite+%E7%B4%A2%E5%BC%95&limit=100');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('incremental search API carries request, cursor, signal and cancel id', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ sessions: [], matches: [], progress: { complete: true } }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const controller = new AbortController();
    await client.listGlobalSessions({
      query: '最近任务',
      limit: 500,
      cursor: '7:50',
      requestId: 'search-7',
      signal: controller.signal,
    });
    await client.searchSessionUserMessages(
      '正文', 50, { requestId: 'search-7', signal: controller.signal },
    );
    await client.cancelSessionSearch('search-7');

    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/session-search/sessions?q=%E6%9C%80%E8%BF%91%E4%BB%BB%E5%8A%A1&limit=100&request_id=search-7&cursor=7%3A50',
    );
    assert.match(calls[1].url, /request_id=search-7/);
    assert.equal(
      calls[2].url,
      'http://127.0.0.1:4567/api/session-search/requests/search-7/cancel',
    );
    assert.equal(calls[2].opts.method, 'POST');
    assert.equal(calls[2].opts.keepalive, true);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('GUI locale API uses the authenticated config endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ locale: 'en-US' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    assert.deepEqual(await client.getUiLocale(), { locale: 'en-US' });
    assert.deepEqual(await client.setUiLocale('auto'), { locale: 'en-US' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/ui-locale');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { locale: 'auto' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('remote Web API reads and writes the authenticated mode endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({
        configured_enabled: true,
        effective_enabled: true,
        connections: [],
      }),
    };
  };
  try {
    const client = createApi({
      origin: 'http://127.0.0.1:4567',
      token: 'remote-token',
    });
    await client.getRemoteWeb();
    await client.setRemoteWeb(true);

    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/config/remote-web',
    );
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(
      calls[0].opts.headers['X-ACECode-Token'],
      'remote-token',
    );
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { enabled: true });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('executeCommand posts to builtin command endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 202,
      headers: { get: () => 'application/json' },
      json: async () => ({ queued: true, command: 'init' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.executeCommand('session/a', {
      command: 'init',
      args: '',
      display_text: '/init',
    });
    assert.deepEqual(result, { queued: true, command: 'init' });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/session%2Fa/commands');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), {
      command: 'init',
      args: '',
      display_text: '/init',
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('askSideQuestion posts the isolated question payload', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ question: 'why?', answer: 'because' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.askSideQuestion('session/a', 'why?');
    assert.deepEqual(result, { question: 'why?', answer: 'because' });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/session%2Fa/side-question');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { question: 'why?' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('opencode import API uses workspace-scoped endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, job_id: 'job/1' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getOpencodeImportPreview('w/a');
    await client.startOpencodeImport('w/a', ['ses/1', 'ses-2']);
    await client.getOpencodeImportJob('w/a', 'job/1');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/opencode-import');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/opencode-import');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { session_ids: ['ses/1', 'ses-2'] });
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/opencode-import/job%2F1');
    assert.equal(calls[2].opts.method, 'GET');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('model probe API keeps upstream probe and local cache reads separate', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ models: ['gpt-4o'] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const draft = {
      catalog_provider_id: 'custom-openai',
      provider: 'openai',
      base_url: 'http://localhost:1234/v1',
      api_key: 'sk',
      request_headers: { 'X-Probe': 'acecode' },
    };
    const result = await client.probeModels(draft);
    await client.getModelProbeCache(draft);
    assert.deepEqual(result, { models: ['gpt-4o'] });
    assert.equal(calls.length, 2);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/models/probe');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), draft);
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/models/probe/cache');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), draft);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('model catalog API keeps summary, limited provider query, and refresh separate', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getModelCatalog();
    await client.queryModelCatalog('open/router', ' luna  ', 500);
    await client.queryModelCatalog('openrouter', '', 25);
    await client.refreshModelCatalog();

    assert.equal(calls.length, 4);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/models/catalog');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(
      calls[1].url,
      'http://127.0.0.1:4567/api/models/catalog/open%2Frouter?q=luna&limit=100',
    );
    assert.equal(calls[1].opts.method, 'GET');
    assert.equal(
      calls[2].url,
      'http://127.0.0.1:4567/api/models/catalog/openrouter?limit=25',
    );
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/models/catalog/refresh');
    assert.equal(calls[3].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[3].opts.body), {});
    for (const call of calls) {
      assert.equal(call.opts.headers['X-ACECode-Token'], 'tok');
    }
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Copilot auth API methods use expected endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, status: 'pending' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getCopilotAuth();
    await client.startCopilotAuth();
    await client.pollCopilotAuth('device-123');
    await client.logoutCopilot();

    assert.equal(calls.length, 4);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/copilot/auth');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/copilot/auth/device');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {});
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/copilot/auth/device/poll');
    assert.equal(calls[2].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[2].opts.body), { device_code: 'device-123' });
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/copilot/auth');
    assert.equal(calls[3].opts.method, 'DELETE');
    for (const call of calls) {
      assert.equal(call.opts.headers['X-ACECode-Token'], 'tok');
    }
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Grok auth API methods use managed device-flow endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, status: 'pending' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getGrokAuth();
    await client.startGrokAuth();
    await client.pollGrokAuth('grok-device-123');
    await client.logoutGrok();

    assert.deepEqual(calls.map((call) => [call.url, call.opts.method]), [
      ['http://127.0.0.1:4567/api/grok/auth', 'GET'],
      ['http://127.0.0.1:4567/api/grok/auth/device', 'POST'],
      ['http://127.0.0.1:4567/api/grok/auth/device/poll', 'POST'],
      ['http://127.0.0.1:4567/api/grok/auth', 'DELETE'],
    ]);
    assert.deepEqual(JSON.parse(calls[2].opts.body), { device_code: 'grok-device-123' });
    for (const call of calls) {
      assert.equal(call.opts.headers['X-ACECode-Token'], 'tok');
    }
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('UI preference API carries the complete appearance contract', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({
        show_acecode_avatar: false,
        theme: 'dark',
        color_theme: 'orange',
        font_size: 'large',
      }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const payload = {
      show_acecode_avatar: false,
      theme: 'dark',
      color_theme: 'orange',
      font_size: 'large',
    };
    const got = await client.getUiPreferences();
    const saved = await client.setUiPreferences(payload);

    assert.deepEqual(got, payload);
    assert.deepEqual(saved, payload);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/ui-preferences');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/ui-preferences');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), payload);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Desktop onboarding API reads status and persists dismissal', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ guide_version: 1, dismissed: calls.length > 1 }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const status = await client.getDesktopOnboarding();
    const dismissed = await client.dismissDesktopOnboarding();

    assert.deepEqual(status, { guide_version: 1, dismissed: false });
    assert.deepEqual(dismissed, { guide_version: 1, dismissed: true });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/ui/onboarding/desktop');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/ui/onboarding/desktop/dismiss');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Custom instructions API reads and writes daemon-backed text', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ text: 'Prefer Chinese replies.' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getCustomInstructions();
    const saved = await client.setCustomInstructions({ text: 'Prefer Chinese replies.' });

    assert.deepEqual(got, { text: 'Prefer Chinese replies.' });
    assert.deepEqual(saved, { text: 'Prefer Chinese replies.' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/custom-instructions');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/custom-instructions');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { text: 'Prefer Chinese replies.' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Connectors API reads and writes daemon-backed connector list', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  const payload = {
    connectors: [
      {
        id: 'alpha-connector',
        name: 'Alpha Connector',
        description: 'Connect alpha providers',
        enabled: true,
      },
    ],
  };
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => payload,
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getConnectors();
    const saved = await client.setConnectors(payload);

    assert.deepEqual(got, payload);
    assert.deepEqual(saved, payload);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/connectors');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/connectors');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), payload);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Pinned session visual order API reads and writes global order endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ items: [{ workspace_hash: 'w1', session_id: 'a' }] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getPinnedSessionOrder();
    const saved = await client.setPinnedSessionOrder([{ workspace_hash: 'w2', session_id: 'b' }]);

    assert.deepEqual(got, { items: [{ workspace_hash: 'w1', session_id: 'a' }] });
    assert.deepEqual(saved, { items: [{ workspace_hash: 'w1', session_id: 'a' }] });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/pinned-sessions/order');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/pinned-sessions/order');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      items: [{ workspace_hash: 'w2', session_id: 'b' }],
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('No-workspace pinned session API reads and writes dedicated endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ no_workspace: true, session_ids: ['task-1'] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getNoWorkspacePinnedSessions();
    const saved = await client.setNoWorkspacePinnedSessions(['task-2']);

    assert.deepEqual(got, { no_workspace: true, session_ids: ['task-1'] });
    assert.deepEqual(saved, { no_workspace: true, session_ids: ['task-1'] });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/no-workspace/pinned-sessions');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/no-workspace/pinned-sessions');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { session_ids: ['task-2'] });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Default permission mode API reads and writes config endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ mode: 'accept-edits' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getDefaultPermissionMode();
    const saved = await client.setDefaultPermissionMode('accept-edits');

    assert.deepEqual(got, { mode: 'accept-edits' });
    assert.deepEqual(saved, { mode: 'accept-edits' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/default-permission-mode');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/default-permission-mode');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { mode: 'accept-edits' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Desktop notification API reads and writes the master switch', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ enabled: false }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.getDesktopNotifications();
    await client.setDesktopNotifications(false);

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/desktop-notifications');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/desktop-notifications');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { enabled: false });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Session model switch and default model use separate endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ name: 'fast' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.switchModel('sid-1', 'fast');
    await client.setDefaultModel('slow');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/sid-1/model');
    assert.equal(calls[0].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { name: 'fast' });
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/default-model');
    assert.equal(calls[1].opts.method, 'POST');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { name: 'slow' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Upgrade config API reads and writes daemon-backed base_url', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ base_url: 'http://2017studio.imwork.net:82/aupdate/' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getUpgradeConfig();
    const saved = await client.setUpgradeConfig({ base_url: 'https://updates.example.test/ace' });

    assert.deepEqual(got, { base_url: 'http://2017studio.imwork.net:82/aupdate/' });
    assert.deepEqual(saved, { base_url: 'http://2017studio.imwork.net:82/aupdate/' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/config/upgrade');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/config/upgrade');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      base_url: 'https://updates.example.test/ace',
    });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Update API checks status and manages GUI update jobs', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: opts.method === 'POST' ? 202 : 200,
      headers: { get: () => 'application/json' },
      json: async () => {
        if (url.endsWith('/api/update/status')) {
          return { status: 'available', update_available: true, latest_version: '9.9.9' };
        }
        if (url.endsWith('/cancel')) {
          return { job_id: 'job-1', state: 'running', cancel_requested: true };
        }
        if (opts.method === 'POST') {
          return { started: true, job_id: 'job-1', target_version: '9.9.9' };
        }
        return { job_id: 'job-1', state: 'running', phase: 'downloading' };
      },
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const status = await client.getUpdateStatus();
    const started = await client.startUpdate();
    const latest = await client.getLatestUpdateJob();
    const job = await client.getUpdateJob('job 1');
    const cancelling = await client.cancelUpdate('job 1');

    assert.deepEqual(status, {
      status: 'available',
      update_available: true,
      latest_version: '9.9.9',
    });
    assert.deepEqual(started, { started: true, job_id: 'job-1', target_version: '9.9.9' });
    assert.deepEqual(latest, { job_id: 'job-1', state: 'running', phase: 'downloading' });
    assert.deepEqual(job, { job_id: 'job-1', state: 'running', phase: 'downloading' });
    assert.deepEqual(cancelling, { job_id: 'job-1', state: 'running', cancel_requested: true });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/update/status');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/update/start');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/update/job');
    assert.equal(calls[2].opts.method, 'GET');
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/update/jobs/job%201');
    assert.equal(calls[3].opts.method, 'GET');
    assert.equal(calls[4].url, 'http://127.0.0.1:4567/api/update/jobs/job%201/cancel');
    assert.equal(calls[4].opts.method, 'POST');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Desktop feedback API methods use dedicated feedback endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => (
        url.includes('recent-sessions')
          ? { sessions: [{ id: 's1' }] }
          : { ok: true, package_filename: 'feedback.zip' }
      ),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const sessions = await client.listDesktopFeedbackSessions(25);
    const submitted = await client.submitDesktopFeedback({
      feedback_text: 'desktop issue',
      session_id: 's1',
      workspace_hash: 'w1',
    });

    assert.deepEqual(sessions, { sessions: [{ id: 's1' }] });
    assert.deepEqual(submitted, { ok: true, package_filename: 'feedback.zip' });
    assert.equal(calls.length, 2);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/feedback/desktop/recent-sessions?limit=25');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/feedback/desktop');
    assert.equal(calls[1].opts.method, 'POST');
    assert.equal(calls[1].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      feedback_text: 'desktop issue',
      session_id: 's1',
      workspace_hash: 'w1',
    });
    assert.ok(!calls.some((call) => call.url.includes('/messages') || call.url.includes('/commands')));
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('workspace-scoped transcript reads carry the workspace hash', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ messages: [] }),
    };
  };
  try {
    const client = createApi({
      origin: 'http://127.0.0.1:4567',
      token: 'tok',
      workspaceHash: 'workspace/hash',
    });
    await client.getMessages('session id', 0);

    assert.equal(calls.length, 1);
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/sessions/session%20id/messages?since=0&workspace=workspace%2Fhash',
    );
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('GUI update checks and starts are exempt from the default 30-second timeout', async () => {
  const previousFetch = globalThis.fetch;
  const previousSetTimeout = globalThis.setTimeout;
  let timerScheduled = false;
  globalThis.setTimeout = () => {
    timerScheduled = true;
    return 93;
  };
  globalThis.fetch = async () => ({
    ok: true,
    status: 202,
    headers: { get: () => 'application/json' },
    json: async () => ({ started: true, job_id: 'job-1' }),
  });
  try {
    const client = createApi({ origin: 'http://acecode.test', token: '' });
    await client.getUpdateStatus();
    await client.startUpdate();
    assert.equal(timerScheduled, false);
  } finally {
    globalThis.fetch = previousFetch;
    globalThis.setTimeout = previousSetTimeout;
  }
});

await run('transcript reads accept a per-session workspace override', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ messages: [] }),
    };
  };
  try {
    const client = createApi({
      origin: 'http://127.0.0.1:4567',
      token: 'tok',
    });
    await client.getMessages('session id', 0, 'workspace/hash');

    assert.equal(calls.length, 1);
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/sessions/session%20id/messages?since=0&workspace=workspace%2Fhash',
    );
  } finally {
    globalThis.fetch = previousFetch;
  }
});

run('trajectory path carries both precise and legacy cursors', () => {
  assert.equal(
    sessionTrajectoryPath('session/id', {
      after: 41,
      legacyAfter: 17,
      limit: 5000,
      workspaceHash: 'workspace/hash',
    }),
    '/api/sessions/session%2Fid/trajectory?after=41&legacy_after=17&limit=1000&workspace=workspace%2Fhash',
  );
});

run('trajectory path carries tail paging boundaries', () => {
  assert.equal(
    sessionTrajectoryPath('session/id', {
      tail: true,
      before: 41,
      legacyBefore: 17,
      limit: 100,
    }),
    '/api/sessions/session%2Fid/trajectory?after=0&legacy_after=0&tail=1&before=41&legacy_before=17&limit=100',
  );
});

await run('trajectory client uses the connection workspace by default', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ records: [], next_after: 0 }),
    };
  };
  try {
    const client = createApi({
      origin: 'http://127.0.0.1:4567',
      token: 'tok',
      workspaceHash: 'workspace/hash',
    });
    await client.getTrajectory('session id', {
      after: 8,
      legacyAfter: 3,
      limit: 50,
    });

    assert.equal(calls.length, 1);
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/sessions/session%20id/trajectory?after=8&legacy_after=3&limit=50&workspace=workspace%2Fhash',
    );
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('getSkillRoot uses workspace query and auth token', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ path: '/repo/.acecode/skills', source: 'project_acecode' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const got = await client.getSkillRoot('workspace/hash');

    assert.deepEqual(got, { path: '/repo/.acecode/skills', source: 'project_acecode' });
    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/skills/root?workspace=workspace%2Fhash');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('listCommands distinguishes explicit no-workspace from omitted workspace', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ builtins: [], commands: [], skills: [] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.listCommands('');
    await client.listCommands();

    assert.equal(calls.length, 2);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/commands?workspace=');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/commands');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('Hook management API methods use encoded hook ids and expected endpoints', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ hooks: [], sources: [] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const hookId = 'project_local:codex_json:C:/repo/.codex/hooks.json::PreToolUse#1.1';
    await client.listHooks();
    await client.refreshHooks();
    await client.trustHook(hookId);
    await client.disableHook(hookId);
    await client.enableHook(hookId);

    assert.equal(calls.length, 5);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/hooks');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/hooks/refresh');
    assert.equal(calls[1].opts.method, 'POST');
    const encoded = encodeURIComponent(hookId);
    assert.equal(calls[2].url, `http://127.0.0.1:4567/api/hooks/${encoded}/trust`);
    assert.equal(calls[2].opts.method, 'POST');
    assert.equal(calls[3].url, `http://127.0.0.1:4567/api/hooks/${encoded}/disable`);
    assert.equal(calls[3].opts.method, 'POST');
    assert.equal(calls[4].url, `http://127.0.0.1:4567/api/hooks/${encoded}/enable`);
    assert.equal(calls[4].opts.method, 'POST');
    for (const call of calls) {
      assert.equal(call.opts.headers['X-ACECode-Token'], 'tok');
    }
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('readFileBlob fetches authenticated binary file content', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'image/png' },
      blob: async () => new Blob(['png-bytes'], { type: 'image/png' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const blob = await client.readFileBlob('/repo root', 'docs/a b.png');

    assert.equal(calls.length, 1);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/files/blob?cwd=%2Frepo%20root&path=docs%2Fa%20b.png');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(blob.type, 'image/png');
    assert.equal(await blob.text(), 'png-bytes');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('editable file client reads a byte revision and sends it back on save', async () => {
  const originalFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({
        text: 'hello\n',
        read_id: calls.length === 1 ? 'rev-1' : 'rev-2',
        encoding: 'utf-8',
        line_ending: 'lf',
      }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const opened = await client.readEditableFile('/repo root', 'docs/a b.md');
    assert.equal(opened.read_id, 'rev-1');
    const saved = await client.saveEditableFile(
      '/repo root',
      'docs/a b.md',
      'changed\n',
      opened.read_id,
    );
    assert.equal(saved.read_id, 'rev-2');
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/files/editable?cwd=%2Frepo%20root&path=docs%2Fa%20b.md',
    );
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/files/editable');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), {
      cwd: '/repo root',
      path: 'docs/a b.md',
      text: 'changed\n',
      read_id: 'rev-1',
    });
  } finally {
    globalThis.fetch = originalFetch;
  }
});

await run('exportSession posts the selected session and workspace identity', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ ok: true, filename: '会话.md' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const result = await client.exportSession('s/1', 'workspace-1');
    assert.deepEqual(result, { ok: true, filename: '会话.md' });
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/s%2F1/export-markdown');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), { workspace_hash: 'workspace-1' });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('listFiles can request hidden and noise entries for path references', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => [],
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.listFiles('/repo root', 'src/deep', true, true);
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/files?cwd=%2Frepo%20root&path=src%2Fdeep&show_hidden=1&show_noise=1',
    );
    assert.equal(calls[0].opts.method, 'GET');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('readFileBlob parses JSON errors as ApiError body', async () => {
  const previousFetch = globalThis.fetch;
  globalThis.fetch = async () => ({
    ok: false,
    status: 415,
    headers: { get: () => 'application/json' },
    json: async () => ({ error: 'file too large', size: 123 }),
  });
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await assert.rejects(
      () => client.readFileBlob('/repo', 'large.png'),
      (err) => {
        assert.equal(err instanceof ApiError, true);
        assert.equal(err.status, 415);
        assert.deepEqual(err.body, { error: 'file too large', size: 123 });
        return true;
      },
    );
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('archive API methods use expected endpoints and archived query flag', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => [],
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    await client.listSessions({ archived: true });
    await client.listWorkspaceSessions('w/a', { archived: true });
    await client.listWorkspaceSessions('w/a', { limit: 5 });
    await client.archiveSession('s/a');
    await client.unarchiveWorkspaceSession('w/a', 's/a');
    await client.purgeArchivedSession('s/a');
    await client.purgeArchivedWorkspaceSession('w/a', 's/a');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions?archived=1');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions?archived=1');
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions?limit=5');
    assert.equal(calls[2].opts.method, 'GET');
    assert.equal(calls[3].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/archive');
    assert.equal(calls[3].opts.method, 'PUT');
    assert.equal(calls[4].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/archive');
    assert.equal(calls[4].opts.method, 'DELETE');
    assert.equal(calls[5].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa?purge=1');
    assert.equal(calls[5].opts.method, 'DELETE');
    assert.equal(calls[6].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa?purge=1');
    assert.equal(calls[6].opts.method, 'DELETE');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('session draft API uses workspace route when workspace hash is available', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ session_id: 's/a', text: 'draft text' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    assert.equal(
      sessionDraftPath('s/a', 'w/a'),
      '/api/workspaces/w%2Fa/sessions/s%2Fa/draft',
    );
    assert.equal(sessionDraftPath('s/a'), '/api/sessions/s%2Fa/draft');

    await client.getSessionDraft('s/a', 'w/a');
    await client.setSessionDraft('s/a', 'draft text', 'w/a');
    await client.setSessionDraft('s/a', '');
    const composerContent = { version: 1, parts: [{ type: 'text', text: 'draft text' }] };
    await client.setSessionDraft('s/a', 'draft text', 'w/a', composerContent);

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/draft');
    assert.equal(calls[0].opts.method, 'GET');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/draft');
    assert.equal(calls[1].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[1].opts.body), { text: 'draft text' });
    assert.equal(calls[2].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/draft');
    assert.equal(calls[2].opts.method, 'PUT');
    assert.deepEqual(JSON.parse(calls[2].opts.body), { text: '' });
    assert.deepEqual(JSON.parse(calls[3].opts.body), { text: 'draft text', composer_content: composerContent });
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('session todos API clears through workspace route when available', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => ({ session_id: 's/a', todos: [] }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    assert.equal(
      sessionTodosPath('s/a', 'w/a'),
      '/api/workspaces/w%2Fa/sessions/s%2Fa/todos',
    );
    assert.equal(sessionTodosPath('s/a'), '/api/sessions/s%2Fa/todos');

    await client.clearSessionTodos('s/a', 'w/a');
    await client.clearSessionTodos('s/a');

    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/workspaces/w%2Fa/sessions/s%2Fa/todos');
    assert.equal(calls[0].opts.method, 'DELETE');
    assert.equal(calls[1].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/todos');
    assert.equal(calls[1].opts.method, 'DELETE');
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('steerTurn posts the expected turn id and structured input payload', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 202,
      headers: { get: () => 'application/json' },
      json: async () => ({ accepted: true, turn_id: 'turn-1' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const payload = {
      text: 'use this constraint',
      attachments: [{ id: 'att-1' }],
      contexts: [{ type: 'selection' }],
      client_message_id: 'queued-s-1',
      expected_turn_id: 'turn-1',
    };
    await client.steerTurn('s/a', payload);

    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/sessions/s%2Fa/turn/steer',
    );
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), payload);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('interruptTurn posts structured input to the immediate interrupt endpoint', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 202,
      headers: { get: () => 'application/json' },
      json: async () => ({ accepted: true, interrupting: true, turn_id: 'turn-1' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const payload = {
      text: 'replace immediately',
      attachments: [{ id: 'att-1' }],
      contexts: [{ type: 'selection' }],
      client_message_id: 'queued-s-2',
      expected_turn_id: 'turn-1',
    };
    await client.interruptTurn('s/a', payload);

    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/sessions/s%2Fa/turn/interrupt',
    );
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), payload);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

// 触发场景:AskUserQuestion 挂起时用户直接在输入框提交文本(插话)。
// 期望:POST 到问题插话端点,body 原样携带 request_id / client_message_id;
// 409 NO_PENDING_QUESTION 要以 ApiError.code 暴露,ChatView 据此退回普通路径。
await run('interjectQuestion posts to the question interject endpoint and exposes NO_PENDING_QUESTION', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  let status = 202;
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    if (status === 202) {
      return {
        ok: true,
        status,
        headers: { get: () => 'application/json' },
        json: async () => ({ accepted: true, turn_id: 'turn-1', request_id: 'rid-1' }),
      };
    }
    return {
      ok: false,
      status,
      headers: { get: () => 'application/json' },
      json: async () => ({ error: 'NO_PENDING_QUESTION', message: 'the question is no longer pending' }),
      text: async () => JSON.stringify({ error: 'NO_PENDING_QUESTION', message: 'the question is no longer pending' }),
    };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' });
    const payload = {
      text: 'neither, use the built-in client',
      request_id: 'rid-1',
      client_message_id: 'interject-s-1',
    };
    const accepted = await client.interjectQuestion('s/a', payload);
    assert.equal(accepted.request_id, 'rid-1');
    assert.equal(
      calls[0].url,
      'http://127.0.0.1:4567/api/sessions/s%2Fa/questions/interject',
    );
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'tok');
    assert.deepEqual(JSON.parse(calls[0].opts.body), payload);

    status = 409;
    await assert.rejects(
      () => client.interjectQuestion('s/a', payload),
      (error) => error.code === 'NO_PENDING_QUESTION' && error.status === 409,
    );
  } finally {
    globalThis.fetch = previousFetch;
  }
});

// 触发场景：刷新包含路径字符的会话；期望：编码路径、鉴权头和响应信封均保持完整。
await run('reloadSessionModel posts to the encoded session route with auth and returns the envelope', async () => {
  const previousFetch = globalThis.fetch;
  const calls = [];
  const envelope = {
    outcome: 'reloaded',
    model_state: { name: 'fast', provider: 'openai', model: 'gpt-5' },
  };
  globalThis.fetch = async (url, opts = {}) => {
    calls.push({ url, opts });
    return {
      ok: true,
      status: 200,
      headers: { get: () => 'application/json' },
      json: async () => envelope,
    };
  };
  try {
    const result = await createApi({ origin: 'http://127.0.0.1:4567', token: 'reload-token' })
      .reloadSessionModel('s/a');

    assert.deepEqual(result, envelope);
    assert.equal(calls[0].url, 'http://127.0.0.1:4567/api/sessions/s%2Fa/model/reload');
    assert.equal(calls[0].opts.method, 'POST');
    assert.equal(calls[0].opts.headers['X-ACECode-Token'], 'reload-token');
    assert.equal(calls[0].opts.body, undefined);
  } finally {
    globalThis.fetch = previousFetch;
  }
});

// 触发场景：刷新端点返回结构化失败；期望：调用方可区分 HTTP 状态与业务错误码。
await run('reloadSessionModel preserves structured non-success errors', async () => {
  const previousFetch = globalThis.fetch;
  globalThis.fetch = async () => ({
    ok: false,
    status: 500,
    headers: { get: () => 'application/json' },
    json: async () => ({ error: 'MODEL_RELOAD_FAILED', message: 'model profile reload failed' }),
  });
  try {
    await assert.rejects(
      createApi({ origin: 'http://127.0.0.1:4567', token: 'tok' })
        .reloadSessionModel('session-1'),
      (error) => error instanceof ApiError
        && error.status === 500
        && error.code === 'MODEL_RELOAD_FAILED',
    );
  } finally {
    globalThis.fetch = previousFetch;
  }
});

await run('theme management requests keep auth in headers and return ZIP bytes without a public token URL', async () => {
  const previousFetch = globalThis.fetch, calls = [];
  const archive = new Blob(['PK theme'], { type: 'application/zip' });
  globalThis.fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: true, status: 200, headers: { get: () => url.endsWith('/download') ? 'application/zip' : 'application/json' },
      blob: async () => archive, json: async () => ({ job_id: 'job/1', state: 'completed' }) };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'theme-token' });
    await client.exportTheme('ai-eva', { native_save: true });
    await client.getThemeExport('job/1');
    await client.cancelThemeExport('job/1');
    assert.equal(await client.readThemeExport('job/1'), archive);
    await client.deleteTheme('ai-eva');
    assert.deepEqual(calls.map(({ url, options }) => [url.replace('http://127.0.0.1:4567', ''), options.method]), [
      ['/api/themes/ai-eva/export', 'POST'], ['/api/themes/exports/job%2F1', 'GET'],
      ['/api/themes/exports/job%2F1/cancel', 'POST'], ['/api/themes/exports/job%2F1/download', 'GET'], ['/api/themes/ai-eva', 'DELETE'],
    ]);
    assert.deepEqual(JSON.parse(calls[0].options.body), { native_save: true });
    for (const call of calls) {
      assert.equal(call.options.headers['X-ACECode-Token'], 'theme-token');
      assert.equal(call.url.includes('theme-token'), false);
    }
  } finally { globalThis.fetch = previousFetch; }
});

await run('native theme Save As has no request timeout and package download accepts cancellation', async () => {
  const previousFetch = globalThis.fetch, previousTimeout = globalThis.setTimeout, previousClear = globalThis.clearTimeout;
  const timers = [], signals = [];
  globalThis.setTimeout = (callback, ms) => { timers.push(ms); return timers.length; };
  globalThis.clearTimeout = () => {};
  globalThis.fetch = async (url, options) => {
    signals.push(options.signal);
    if (url.endsWith('/download')) return new Promise((resolve, reject) => options.signal.addEventListener('abort', () => reject(Object.assign(new Error('cancelled'), { name: 'AbortError' }))));
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ state: 'cancelled', cancelled: true }) };
  };
  try {
    const client = createApi({ origin: 'http://127.0.0.1:4567', token: 'theme-token' });
    await client.exportTheme('ai-eva', { native_save: true });
    assert.deepEqual(timers, []);
    await client.exportTheme('ai-eva');
    assert.deepEqual(timers, [DEFAULT_REQUEST_TIMEOUT_MS]);
    const abort = new AbortController();
    const downloading = client.readThemeExport('job/1', { signal: abort.signal });
    abort.abort();
    await assert.rejects(downloading, (error) => error.name === 'AbortError');
  } finally {
    globalThis.fetch = previousFetch; globalThis.setTimeout = previousTimeout; globalThis.clearTimeout = previousClear;
  }
});

await run('session reasoning mutation encodes the session ID and sends explicit override or null', async () => {
  const original = globalThis.fetch;
  const calls = [];
  globalThis.fetch = async (url, options) => {
    calls.push({ url, options });
    return { ok: true, status: 200, headers: { get: () => 'application/json' }, json: async () => ({ name: 'fake', reasoning_effort: JSON.parse(options.body).effort }) };
  };
  try {
    const api = createApi({ origin: 'http://127.0.0.1:49000', token: 'fake-token' });
    const selected = await api.setSessionReasoning('session/one', 'high');
    const reset = await api.setSessionReasoning('session/one', null);
    assert.equal(calls[0].url, 'http://127.0.0.1:49000/api/sessions/session%2Fone/reasoning');
    assert.equal(calls[0].options.method, 'POST');
    assert.equal(calls[0].options.headers['X-ACECode-Token'], 'fake-token');
    assert.deepEqual(JSON.parse(calls[0].options.body), { effort: 'high' });
    assert.deepEqual(JSON.parse(calls[1].options.body), { effort: null });
    assert.equal(selected.reasoning_effort, 'high');
    assert.equal(reset.reasoning_effort, null);
  } finally { globalThis.fetch = original; }
});
