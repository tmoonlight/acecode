import assert from 'node:assert/strict';
import { createSessionTitleAnimationStore, sessionTitleAnimationKey, titleGraphemes } from './sessionTitleAnimation.js';

function harness() {
  let time = 0;
  let nextId = 0;
  let animate = true;
  let reduced = false;
  const scheduled = new Map();
  const store = createSessionTitleAnimationStore({
    now: () => time,
    schedule: (callback) => { scheduled.set(++nextId, callback); return nextId; },
    cancel: (id) => scheduled.delete(id),
    shouldAnimate: () => animate,
    reducedMotion: () => reduced,
  });
  return { store, scheduled,
    reduceMotion: () => { reduced = true; },
    hide: () => { animate = false; },
    advance(ms) {
      time += ms;
      const callbacks = [...scheduled.values()];
      scheduled.clear();
      callbacks.forEach((callback) => callback());
    },
  };
}

function test(name, fn) {
  fn();
  console.log(`[pass] ${name}`);
}

test('title animation shares progress, ignores duplicate updates and removes cursor at completion', () => {
  const h = harness();
  let top, sidebar;
  const a = h.store.subscribe('session', () => { top = h.store.getSnapshot('session'); });
  const b = h.store.subscribe('session', () => { sidebar = h.store.getSnapshot('session'); });
  assert.equal(h.store.getSnapshot('session'), null);
  h.store.start('session', '修改会话标题的效果');
  assert.equal(top.text, '修');
  assert.equal(top, sidebar);
  h.advance(100);
  const partial = top;
  assert.ok(partial.text.length > 1 && partial.typing);
  h.store.start('session', '修改会话标题的效果');
  assert.equal(top, partial);
  assert.equal(h.scheduled.size, 1);
  h.advance(550);
  assert.equal(top.text, '修改会话标题的效果');
  assert.equal(top.typing, false);
  assert.equal(top, sidebar);
  assert.equal(h.scheduled.size, 0);
  h.store.start('session', top.title);
  assert.equal(h.scheduled.size, 0);
  a(); b();
  assert.equal(h.store.getSnapshot('session'), null);
});

test('new title interrupts old title even when both start with the same character', () => {
  const h = harness();
  const release = h.store.subscribe('session', () => {});
  h.store.start('session', '标题甲');
  h.store.start('session', '标题乙');
  assert.equal(h.store.getSnapshot('session').title, '标题乙');
  h.advance(90);
  assert.equal(h.store.getSnapshot('session').text, '标题');
  release();
  assert.equal(h.scheduled.size, 0);
  assert.equal(h.store.getSnapshot('session'), null);
});

test('Unicode graphemes remain whole and long titles finish within the cap', () => {
  assert.deepEqual(titleGraphemes('中e\u0301\u{1F469}\u200D\u{1F4BB}'), ['中', 'e\u0301', '\u{1F469}\u200D\u{1F4BB}']);
  const h = harness();
  h.store.subscribe('session', () => {});
  const title = '中文'.repeat(200);
  h.store.start('session', title);
  h.advance(650);
  assert.deepEqual(h.store.getSnapshot('session'), { title, text: title, typing: false });
});

test('hidden completion stops the animation clock', () => {
  const h = harness();
  h.store.subscribe('session', () => {});
  h.store.start('session', '正在生成标题');
  h.store.finish('session');
  assert.equal(h.store.getSnapshot('session').typing, false);
  assert.equal(h.scheduled.size, 0);
  h.hide();
  h.store.start('session', '直接显示完整标题');
  assert.equal(h.store.getSnapshot('session').text, '直接显示完整标题');
  assert.equal(h.store.getSnapshot('session').typing, false);
  assert.equal(h.scheduled.size, 0);
});

test('reduced motion retains synchronized partial text and finishes within 320ms', () => {
  const h = harness();
  let top, sidebar;
  h.store.subscribe('session', () => { top = h.store.getSnapshot('session'); });
  h.store.subscribe('session', () => { sidebar = h.store.getSnapshot('session'); });
  h.reduceMotion();
  const title = '减少动态效果仍然需要看到会话标题的逐字反馈';
  h.store.start('session', title);
  h.advance(100);
  assert.equal(top, sidebar);
  assert.equal(top.typing, true);
  assert.ok(top.text.length > 1 && top.text.length < title.length);
  h.advance(220);
  assert.deepEqual(top, {title, text:title, typing:false});
  assert.equal(h.scheduled.size, 0);
});

test('title identities normalize wire fields and isolate workspace and no-workspace sessions', () => {
  assert.equal(sessionTitleAnimationKey(null), '');
  assert.equal(sessionTitleAnimationKey({ id: '1', workspace_hash: 'work' }),
    sessionTitleAnimationKey({ sessionId: '1', workspaceHash: 'work', port: 28080 }));
  assert.equal(sessionTitleAnimationKey({ id: '1', no_workspace: true, workspace_hash: 'stale' }),
    sessionTitleAnimationKey({ sessionId: '1', noWorkspace: true }));
  assert.notEqual(sessionTitleAnimationKey({ id: '1', workspace_hash: 'a' }),
    sessionTitleAnimationKey({ id: '1', workspace_hash: 'b' }));
});
