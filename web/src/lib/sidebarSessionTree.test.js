import assert from 'node:assert/strict';
import { sidebarSessionProjection, sidebarSessionRevealLimit } from './sidebarSessions.js';
import { normalizeWorkspaceSessionListResponse } from './sidebarWorkspaceSessions.js';
import { applySidebarSessionStatuses, mergeSidebarSessionStatuses, sidebarSessionRoots, sidebarSessionStatusRows, sidebarTreeDropTarget, validSidebarTreeDrop } from './sidebarSessionTree.js';
import { applyStatusUpdate } from './sessionStatus.js';

const group = { id: 'new', workspace_hash: 'w', sidebar_key: 'loop:daily', sidebar_rank: 0, sidebar_child_count: 12 };
const history = Array.from({ length: 12 }, (_, i) => ({
  id: `old-${i}`, workspace_hash: 'w', sidebar_key: `session:old-${i}`,
  sidebar_parent_key: 'loop:daily', sidebar_rank: i,
}));
const ordinary = Array.from({ length: 6 }, (_, i) => ({ id: `n-${i}`, sidebar_rank: i + 1 }));
const rows = [group, ...history, ...ordinary];
const projection = sidebarSessionProjection(rows);
assert.equal(projection.visibleSessions.length, 5);
assert.equal(projection.visibleSessions[0].sidebar_children.length, 12);
assert.equal(projection.hiddenCount, 2);
assert.equal(sidebarSessionRoots(rows).length, 7);
assert.equal(sidebarSessionRevealLimit(rows, { sessionId: 'old-11', workspaceHash: 'w' }), 5);
const page = normalizeWorkspaceSessionListResponse({ sessions: rows, sidebar_tree: true, total: 7, visible_roots: 5, has_more: true });
assert.equal(page.total, 7);
assert.equal(page.visibleRoots, 5);

const parent = { key: 'loop:parent', parent: '', scheduled: true, childCount: 1, left: 10, top: 0, bottom: 40 };
const child = { key: 'session:child', parent: parent.key, scheduled: true, childCount: 0, left: 35, top: 40, bottom: 80 };
const other = { key: 'session:other', parent: '', scheduled: false, childCount: 0, left: 10, top: 80, bottom: 120 };
const single = { key: 'loop:single', parent: '', scheduled: true, childCount: 0, left: 10, top: 120, bottom: 160 };
const targets = [parent, child, other, single];
assert.equal(validSidebarTreeDrop(parent, other, 'inside'), false);
assert.equal(validSidebarTreeDrop(parent, single, 'inside'), false);
assert.equal(validSidebarTreeDrop(other, child, 'inside'), false);
assert.equal(validSidebarTreeDrop(parent, child, 'before', parent), false);
assert.equal(validSidebarTreeDrop(parent, other, 'before'), true);
assert.equal(validSidebarTreeDrop(child, other, 'inside'), false);
assert.equal(validSidebarTreeDrop(single, other, 'inside'), false);
assert.equal(validSidebarTreeDrop(child, single, 'inside'), true);
assert.equal(validSidebarTreeDrop(other, single, 'inside'), true);
assert.equal(validSidebarTreeDrop(other, { ...child, parent: '' }, 'inside'), true);
assert.equal(validSidebarTreeDrop(other, child, 'before', parent), true);
assert.equal(validSidebarTreeDrop(other, child, 'before', { ...parent, scheduled: false }), false);
assert.equal(validSidebarTreeDrop(other, child, 'before'), false);
assert.equal(validSidebarTreeDrop(child, parent, 'after'), true);
assert.equal(validSidebarTreeDrop(child, child, 'after'), false);
assert.equal(validSidebarTreeDrop(single, parent, 'inside'), true);
assert.deepEqual(sidebarTreeDropTarget(targets, child, 20, 60), { ...parent, placement: 'after' });
assert.equal(sidebarTreeDropTarget(targets, child, 60, 100), null);
assert.deepEqual(sidebarTreeDropTarget(targets, child, 60, 82), { ...other, placement: 'before' });
assert.deepEqual(sidebarTreeDropTarget(targets, child, 60, 140), { ...single, placement: 'inside' });
assert.deepEqual(sidebarTreeDropTarget(targets, other, 60, 42), { ...child, placement: 'before' });
assert.equal(sidebarTreeDropTarget(targets, parent, 60, 140), null);
assert.equal(validSidebarTreeDrop({ ...single, group: true }, parent, 'inside'), false);

const taskGroup = {
  id: 'sidebar-group:loop:daily', sidebar_group: true, sidebar_group_name: 'Daily task',
  sidebar_key: 'loop:daily', busy: false, status: 'idle',
  sidebar_members: [
    { id: 'older', attention_state: 'unread', busy: false },
    { id: 'latest', attention_state: 'in_progress', busy: true },
  ],
};
let status = applySidebarSessionStatuses(new Map(), [taskGroup]);
assert.equal(status.has(taskGroup.id), false, 'group headers are not real session status owners');
assert.equal(sidebarSessionStatusRows([taskGroup, { id: 'latest', attention_state: 'in_progress', busy: true }]).length, 2);
let aggregate = mergeSidebarSessionStatuses([taskGroup], status)[0];
assert.equal(aggregate.attention_state, 'unread');
assert.equal(aggregate.busy, false);
status = applyStatusUpdate(status, { id: 'older', state: 'read' });
aggregate = mergeSidebarSessionStatuses([taskGroup], status)[0];
assert.equal(aggregate.attention_state, 'read', 'running children do not make their parent spin or turn green');
assert.equal(aggregate.status, 'idle');
status = applyStatusUpdate(status, { id: 'latest', state: 'unread' });
assert.equal(mergeSidebarSessionStatuses([taskGroup], status)[0].attention_state, 'unread', 'collapsed groups update directly from member events');
status = applyStatusUpdate(status, { id: 'latest', state: 'read' });
assert.equal(mergeSidebarSessionStatuses([taskGroup], status)[0].attention_state, 'read');
console.log('[pass] sidebar tree counts roots, reveals history and only accepts children under scheduled roots');
