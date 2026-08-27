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

test('workspace groups wire drag only to ordinary workspace session rows', () => {
  const sidebar = source('components/Sidebar.jsx');
  const workspaceStart = sidebar.indexOf('function WorkspaceGroup({');
  const noWorkspaceStart = sidebar.indexOf('\nfunction NoWorkspaceSessionGroup(', workspaceStart);
  const sidebarStart = sidebar.indexOf('\nexport function Sidebar(', noWorkspaceStart);
  assert.ok(workspaceStart >= 0 && noWorkspaceStart > workspaceStart && sidebarStart > noWorkspaceStart);

  const workspaceGroup = sidebar.slice(workspaceStart, noWorkspaceStart);
  const noWorkspaceGroup = sidebar.slice(noWorkspaceStart, sidebarStart);
  assert.match(workspaceGroup, /workspaceReorderable/);
  assert.match(workspaceGroup, /onWorkspacePointerDown=\{onWorkspacePointerDown\}/);
  assert.match(workspaceGroup, /workspaceDragState\?\.sourceKey === rowKey/);
  assert.doesNotMatch(noWorkspaceGroup, /workspaceReorderable|onWorkspacePointerDown|workspaceDragState/);

  assert.match(sidebar, /data-sidebar-workspace-session-key=\{workspaceReorderable/);
  assert.match(sidebar, /data-sidebar-workspace-session-workspace=\{workspaceReorderable/);
  assert.match(sidebar, /workspaceDropTargetForPointer\(clientY, drag\.workspaceHash\)/);
  assert.match(
    sidebar,
    /\.filter\(\(row\) => row\.dataset\.sidebarWorkspaceSessionWorkspace === workspace\)/,
  );
});

test('workspace drops update sidebar state while automatic reconciliation stays active', () => {
  const sidebar = source('components/Sidebar.jsx');
  const finishStart = sidebar.indexOf('const finishWorkspaceDrag = useCallback');
  const finishEnd = sidebar.indexOf('\n  const handleWorkspacePointerDown', finishStart);
  assert.ok(finishStart >= 0 && finishEnd > finishStart);
  const finishWorkspaceDrag = sidebar.slice(finishStart, finishEnd);

  assert.match(
    finishWorkspaceDrag,
    /setSessions\(\(prev\) => reorderSidebarWorkspaceSession\(\s*prev,\s*drag\.workspaceHash,\s*drag\.sourceId,\s*drag\.targetId,\s*drag\.placement \|\| 'before'/,
  );
  assert.doesNotMatch(finishWorkspaceDrag, /api\.|setPinnedSessions|setPinnedSessionOrder/);
  assert.match(sidebar, /setSessions\(\(prev\) => retainUnrefreshedSidebarSessions\(prev, incoming,/);
  assert.match(sidebar, /promoteToTop: detail\.reason === 'session-created'/);
});

test('workspace drag shares pinned feedback and suppresses the release click', () => {
  const sidebar = source('components/Sidebar.jsx');
  const styles = source('styles/globals.css');

  assert.match(sidebar, /Math\.hypot\(dx, dy\) < SESSION_DRAG_START_PX/);
  assert.match(sidebar, /className="ace-sidebar-session-drag-ghost"/);
  assert.match(sidebar, /suppressSessionClickRef\.current = true/);
  assert.match(styles, /\.ace-sidebar-pinned-session-row,\s*\.ace-sidebar-workspace-session-row\s*\{\s*cursor: grab;/);
  assert.match(styles, /\.ace-sidebar-workspace-session-row\.is-drop-before::before/);
  assert.match(styles, /\.ace-sidebar-session-drag-ghost\s*\{/);
});
