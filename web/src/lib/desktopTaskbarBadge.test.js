import assert from 'node:assert/strict';
import { createTaskbarBadgeController, desktopTaskbarBadgeAvailable, taskbarBadgeColors } from './desktopTaskbarBadge.js';

function harness(bridge) {
  const calls = [];
  const tokens = { '--ace-accent': '#7c3aed', '--ace-bg': '#f5f5f2', '--ace-fg': '#1a1a1a' };
  const win = {
    aceDesktop_setTaskbarBadge: bridge || ((payload) => { calls.push(payload); return { ok: true }; }),
    getComputedStyle: () => ({ getPropertyValue: (name) => tokens[name] }),
  };
  let scheduled = 0;
  const badge = createTaskbarBadgeController({
    getWindow: () => win,
    getDocument: () => ({ documentElement: {} }),
    schedule: () => ++scheduled,
    cancel: () => {},
  });
  return { badge, calls, tokens, win, scheduled: () => scheduled };
}

const unread = (id, fields = {}) => ({ id, attention_state: 'unread', ...fields });

{
  const { badge, calls, scheduled } = harness();
  badge.retainWorkspaces(['expanded', 'collapsed']);
  badge.replaceScope('expanded', [unread('a'), unread('a'), unread('child', { parent_session_id: 'a' })]);
  badge.replaceScope('collapsed', [unread('b'), unread('running', { busy: true }), unread('archived', { archived: true })]);
  badge.replaceScope('', [unread('free')]);
  assert.equal(scheduled(), 1, 'coalesce status bursts');
  assert.equal(badge.count(), 3, 'all workspaces plus workspace-free tasks, unique roots only');
  badge.flush();
  assert.equal(calls.at(-1).count, 3);
  badge.handleMessage({ type: 'mark_session_read_ack', payload: { session_id: 'a', state: 'read' } });
  badge.updateStatus({ session_id: 'unknown-child', state: 'unread' });
  badge.flush();
  assert.equal(calls.at(-1).count, 2);
  badge.handleMessage({ type: 'session_status_snapshot', payload: { workspace_hash: 'collapsed', sessions: [] } });
  badge.replaceScope('', []);
  badge.flush();
  assert.deepEqual(calls.at(-1), { count: 0 }, 'deleted/archived tasks removed by snapshots');
  assert.equal(badge.flush(), false, 'duplicate state is not sent');
}

{
  const { badge, calls, tokens } = harness();
  badge.replaceScope('project', Array.from({ length: 120 }, (_, i) => unread(`s${i}`)));
  badge.flush();
  assert.equal(calls.at(-1).count, 120, 'native layer formats 99+');
  assert.equal(calls.at(-1).background, '#7c3aed');
  tokens['--ace-accent'] = '#a3e635';
  tokens['--ace-bg'] = '#0f0f0f';
  badge.refresh();
  badge.flush();
  assert.equal(calls.at(-1).count, 120);
  assert.equal(calls.at(-1).background, '#a3e635');
  assert.equal(calls.at(-1).foreground, '#0f0f0f', 'legible theme foreground');
  badge.retainWorkspaces([]);
  badge.replaceScope('project', [unread('late-response')]);
  badge.flush();
  assert.deepEqual(calls.at(-1), { count: 0 }, 'removed workspace cannot reappear from a late snapshot');
}

{
  const { badge } = harness();
  badge.replaceScope('p', [unread('s', { cursor: 7, read_cursor: 6 })]);
  badge.updateStatus({ session_id: 's', state: 'read', cursor: 7, read_cursor: 7, timestamp_ms: 200 });
  badge.replaceScope('p', [unread('s', { cursor: 7, read_cursor: 6 })]);
  assert.equal(badge.count(), 0, 'stale list response cannot undo a read acknowledgement');
  badge.updateStatus({ session_id: 's', state: 'unread', cursor: 8, timestamp_ms: 300 });
  badge.updateStatus({ session_id: 's', state: 'read', cursor: 7, timestamp_ms: 100 });
  assert.equal(badge.count(), 1, 'older status cannot erase a newer unread result');
  badge.removeSession('s');
  assert.equal(badge.count(), 0, 'immediate archive removes the badge');
}

{
  const { badge, calls, win } = harness();
  delete win.aceDesktop_setTaskbarBadge;
  assert.equal(desktopTaskbarBadgeAvailable(win), false);
  badge.replaceScope('p', [unread('s')]);
  badge.refresh();
  assert.equal(badge.flush(), false);
  assert.equal(badge.count(), 0);
  assert.equal(calls.length, 0);
  assert.equal(taskbarBadgeColors({ getPropertyValue: () => 'invalid' }), null);
}

{
  let attempts = 0;
  const { badge } = harness(() => { attempts++; throw new Error('bridge unavailable'); });
  badge.replaceScope('p', [unread('s')]);
  assert.equal(badge.flush(), false);
  assert.equal(badge.flush(), false);
  assert.equal(attempts, 2, 'failed bridge state can be retried');
}

console.log('desktopTaskbarBadge tests passed');

{
  const { badge, calls } = harness();
  const read = { id: 'restored', attention_state: 'read', cursor: 7, read_cursor: 7, timestamp_ms: 100 };
  const manual = unread('restored', { cursor: 7, read_cursor: 6, manual_unread: true, timestamp_ms: 200 });
  badge.replaceScope('', [read]);
  badge.replaceScope('', [manual]);
  badge.flush();
  assert.equal(calls.at(-1).count, 1, 'a newer manual unread snapshot restores tray/taskbar indicators');
  badge.replaceScope('', [read]);
  assert.equal(badge.count(), 1, 'an older list cannot erase manual unread');
  badge.replaceScope('', [{ ...read, timestamp_ms: 300 }]);
  badge.flush();
  assert.deepEqual(calls.at(-1), { count: 0 }, 'a newer read snapshot clears both indicators');
}
