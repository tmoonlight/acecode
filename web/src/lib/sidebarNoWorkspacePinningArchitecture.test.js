import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

test('无工作区任务行复用可访问的置顶控件而不是显式禁用', () => {
  const sidebar = source('components/Sidebar.jsx');
  const groupStart = sidebar.indexOf('function NoWorkspaceSessionGroup({');
  const groupEnd = sidebar.indexOf('\nexport function Sidebar(', groupStart);
  assert.ok(groupStart >= 0 && groupEnd > groupStart);
  const group = sidebar.slice(groupStart, groupEnd);

  assert.match(group, /onTogglePin,/);
  assert.match(group, /onTogglePin=\{onTogglePin\}/);
  assert.doesNotMatch(group, /pinEnabled=\{false\}/);
  assert.match(sidebar, /data-desktop-session-no-workspace=\{noWorkspace \? 'true' : undefined\}/);
});

test('置顶投影包含任务且普通任务分组只接收未置顶任务', () => {
  const sidebar = source('components/Sidebar.jsx');

  assert.match(
    sidebar,
    /pinnedSessionsForList\(renderedSessions, pinnedByWorkspace, pinnedOrderItems\)/,
  );
  assert.match(
    sidebar,
    /const unpinnedNoWorkspaceSessions = useMemo\([\s\S]*?filterPinnedSessions\(noWorkspaceSessions, pinnedByWorkspace\)/,
  );
  assert.match(
    sidebar,
    /sidebarSectionCounts\(\{[\s\S]*?noWorkspaceSessions: unpinnedNoWorkspaceSessions,[\s\S]*?workspaces,/,
  );
  assert.match(sidebar, /<NoWorkspaceSessionGroup[\s\S]*?sessions=\{unpinnedNoWorkspaceSessions\}/);
  assert.match(sidebar, /<NoWorkspaceSessionGroup[\s\S]*?onTogglePin=\{togglePinnedSession\}/);
});

test('任务置顶刷新、保存、右键和拖拽都使用专用 scope', () => {
  const sidebar = source('components/Sidebar.jsx');

  assert.match(sidebar, /const NO_WORKSPACE_SESSION_LIST_KEY = NO_WORKSPACE_PIN_SCOPE;/);
  assert.match(sidebar, /api\.getNoWorkspacePinnedSessions\(\)/);
  assert.match(sidebar, /return api\.setNoWorkspacePinnedSessions\(ids\);/);
  assert.match(sidebar, /no_workspace: !!detail\.noWorkspace/);
  assert.match(sidebar, /const workspaceHash = sessionPinScope\(session\);/);
  assert.match(sidebar, /data-sidebar-pinned-workspace=\{pinned \? pinScope \|\| undefined : undefined\}/);
});
