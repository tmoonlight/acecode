import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function section(text, startMarker, endMarker) {
  const start = text.indexOf(startMarker);
  const end = text.indexOf(endMarker, start + startMarker.length);
  assert.ok(start >= 0 && end > start, `missing section ${startMarker}`);
  return text.slice(start, end);
}

function test(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

test('workspace new-task actions route to the shared lazy home entry point', () => {
  const app = source('App.jsx');
  const sidebar = source('components/Sidebar.jsx');
  const openFlow = section(
    sidebar,
    'const openNewTaskInWorkspace = useCallback((ws) => {',
    '\n  const onAddWorkspace = async',
  );

  assert.match(app, /onOpenHome=\{openHomeForWorkspace\}/);
  assert.match(openFlow, /onOpenHome\?\.\(ws\)/);
  assert.doesNotMatch(openFlow, /api\.create(?:Workspace)?Session|notifySessionListChanged/);
  assert.match(sidebar, /onNewSession=\{openNewTaskInWorkspace\}/);
  assert.doesNotMatch(sidebar, /createSessionInWorkspace|workspaceSessionCreationGuardRef|pendingWorkspaceSessionKeys/);
});

test('chat fork is single-flight and the originating message owns persistent loading feedback', () => {
  const chat = source('components/ChatView.jsx');
  const message = source('components/Message.jsx');
  const styles = source('styles/globals.css');
  const forkFlow = section(
    chat,
    'const forkAndSwitch = useCallback(async (messageId) => {',
    '\n\n  useEffect(() => {',
  );

  assert.match(chat, /forkActionGuardRef = useRef\(createPendingActionGuard\(\)\)/);
  assert.match(
    forkFlow,
    /if \(!forkActionGuardRef\.current\.acquire\(FORK_ACTION_KEY\)\) return;\s*setForkingMessageId\(sourceMessageId\);/,
  );
  assert.ok(
    forkFlow.indexOf('.acquire(FORK_ACTION_KEY)') < forkFlow.indexOf('api.forkSession'),
    'fork guard must acquire before the API call',
  );
  assert.match(
    forkFlow,
    /finally \{\s*forkActionGuardRef\.current\.release\(FORK_ACTION_KEY\);\s*setForkingMessageId/,
  );
  assert.match(
    chat,
    /action !== DESKTOP_CONTEXT_ACTIONS\.FORK_MESSAGE[\s\S]*forkAndSwitch\(target\.messageId\)/,
  );
  assert.ok(
    (chat.match(/forkPending=\{forkingMessageId !== ''\}/g) || []).length >= 2,
    'both transcript message render paths must disable fork actions while pending',
  );
  assert.ok(
    (chat.match(/forkLoading=\{forkingMessageId !== '' && forkingMessageId === String\(/g) || []).length >= 2,
    'both transcript message render paths must identify the loading message',
  );

  assert.match(message, /if \(!messageId \|\| forkPending\) return;/);
  assert.match(message, /disabled=\{!messageId \|\| forkPending\}/);
  assert.match(message, /aria-busy=\{forkLoading \? 'true' : undefined\}/);
  assert.match(message, /forkLoading\s*\? <span className="ace-spinner w-3\.5 h-3\.5"/);
  assert.match(styles, /\.ace-msg-actions\[data-fork-loading="true"\]\s*\{\s*width: 54px;\s*opacity: 1;/);
  assert.match(
    styles,
    /\.ace-msg-actions button\[data-fork-loading="true"\]:disabled\s*\{[\s\S]*?cursor: wait;[\s\S]*?opacity: 1;/,
  );
});
