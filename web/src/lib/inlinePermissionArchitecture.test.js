import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseSync } from '@babel/core';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex);
  assert.notEqual(startIndex, -1, `missing start marker: ${start}`);
  assert.notEqual(endIndex, -1, `missing end marker: ${end}`);
  return text.slice(startIndex, endIndex);
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('App hooks run before authentication branches can return early', () => {
  const ast = parseSync(source('App.jsx'), {
    configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] },
  });
  const app = ast.program.body.find((node) => (
    node.type === 'ExportNamedDeclaration' && node.declaration?.id?.name === 'App'
  ))?.declaration;
  assert.ok(app, 'the production App component must exist');

  // Callback/helper returns do not return from App, and their hooks have a
  // separate lifetime. Inspect only control flow in the component itself.
  function componentNodes(node) {
    if (!node || typeof node !== 'object') return [];
    if (/Function|Method/.test(node.type || '')) return [];
    return [node, ...Object.values(node).flatMap((value) => (
      Array.isArray(value) ? value.flatMap(componentNodes) : componentNodes(value)
    ))];
  }

  let canReturnEarly = false;
  for (const statement of app.body.body) {
    const nodes = componentNodes(statement);
    const hooks = nodes.filter((node) => node.type === 'CallExpression'
      && node.callee.type === 'Identifier' && /^use[A-Z]/.test(node.callee.name));
    assert.ok(!canReturnEarly || hooks.length === 0,
      `App calls ${hooks.map((node) => node.callee.name).join(', ')} after an early return`);
    if (nodes.some((node) => node.type === 'ReturnStatement')) canReturnEarly = true;
  }
});

run('permission cards are chat rows inside the transcript before activity', () => {
  const chat = source('components/ChatView.jsx');
  const renderer = source('components/TranscriptItems.jsx');
  const transcript = between(
    chat,
    'className="ace-chat-transcript-scroll',
    '<StickyUserContext',
  );
  assert.match(transcript, /permissionRequests\.map/);
  assert.match(transcript, /ace-chat-row-assistant-gutter/);
  assert.match(transcript, /data-chat-kind="permission"/);
  assert.match(transcript, /<PermissionCard/);
  // 权限卡片之后只剩插话气泡:会话空闲时的后台任务进度行已经去掉(状态挂在工具行上),
  // 不能再有东西排在权限卡片后面抢「还在跑」的视觉。
  assert.ok(
    transcript.indexOf('permissionRequests.map') < transcript.indexOf('<SteeredMessages'),
    'permission card must appear before the steered messages',
  );
  assert.doesNotMatch(transcript, /CONVERSATION_ACTIVITY_KIND\.BACKGROUND|<ActivityLine/);
  assert.match(renderer, /activityKind === CONVERSATION_ACTIVITY_KIND\.PERMISSION[\s\S]*?CONVERSATION_ACTIVITY_KIND\.QUESTION/);
  assert.doesNotMatch(chat, /ActivityIndicator|data-conversation-activity-bubble/);
  assert.match(chat, /permissionRequests,\s*questionRequest: questionForView/);
});

run('permission geometry participates in tail measurement without changing follow policy', () => {
  const chat = source('components/ChatView.jsx');
  const tailFollowEffect = between(
    chat,
    '// 只在用户仍跟随底部时自动滚到底',
    'useEffect(() => observeChatTailContent',
  );
  assert.match(tailFollowEffect, /permissionRequests/);
  assert.match(tailFollowEffect, /scheduleTailFollowScroll/);
  assert.match(tailFollowEffect, /cancelTailFollowScroll/);
  assert.match(
    chat,
    /\[permissionRequests, scheduleTranscriptMeasures\]/,
  );
  assert.match(chat, /shouldAutoFollowChatTail\(tailFollowStateRef\.current\)/);
});

run('App reconciles server close events and sends decisions to the request session', () => {
  const app = source('App.jsx');
  assert.match(app, /msg\.type === 'permission_request'/);
  assert.match(app, /msg\.type === 'permission_closed'/);
  assert.match(app, /closePermissionRequest\(prev, payload/);
  assert.match(app, /markPermissionSubmitting\(prev, request\.request_id, choice\)/);
  assert.match(
    app,
    /connection\.sendDecision\(request\.request_id, choice, request\.session_id\)/,
  );
  assert.match(app, /clearResolvedPermissionRequests\(prev, sessionId\)/);
  assert.match(app, /payload\.reason === 'permission_timeout'/);
  assert.match(app, /!permissionTimeoutDiagnostic/);
  assert.doesNotMatch(app, /PermissionModal/);
});

run('permission is conversation-scoped and is not a global focus/search/tour blocker', () => {
  const app = source('App.jsx');
  assert.match(app, /visiblePermissionRequests\(permReqs, activeId, permissionOwnership\)/);
  assert.match(app, /visibleQuestionRequest\(questionReqs, activeId, permissionOwnership\)/);
  // 权限未解决时问题必须让位(permission 优先于 question);memoize 后以提前 return 表达。
  assert.match(app, /const visibleQuestionReq = useMemo\(/);
  assert.match(app, /if \(visiblePermissionUnresolved\) return null/);
  assert.match(app, /permissionOpen: false/);

  const tourBlock = between(app, 'const guidedTourBlocked', 'useEffect(() => initInactiveSelection');
  assert.doesNotMatch(tourBlock, /permReq/);
  const focusBlock = between(
    app,
    'const autoFocusChatOnDesktopWindowFocus',
    'const conversationFindEnabled',
  );
  assert.doesNotMatch(focusBlock, /permReq|visiblePermission/);
});

run('known subagent permission and question cards route through the parent without native notifications', () => {
  const app = source('App.jsx');
  assert.match(app, /subagentDirectory/);
  assert.match(app, /conversationOwnerForSession\(sessionId, payload\)/);
  assert.match(app, /pushPermissionRequest\(prev, payload, \{/);
  assert.match(app, /addPendingQuestionRequest\(prev, payload, \{/);
  assert.match(app, /ownerSessionId: ownerSessionId !== sessionId \? ownerSessionId : ''/);
  assert.match(app, /origin_label: permissionOriginLabel\(entry, permissionOwnership\)/);
  assert.match(app, /origin_label: questionOriginLabel\(request, permissionOwnership\)/);
  assert.doesNotMatch(app, /sendDesktopNotification\(['"](?:permission|question)['"]/);
});

console.log('inlinePermissionArchitecture tests passed');
