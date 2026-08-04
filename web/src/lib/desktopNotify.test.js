// desktopNotify.js 单测。
//
// 项目无 JSDOM,只测纯函数(buildNotificationPayload / truncateForNotification /
// shouldSuppress)。notify / maybeNotify 依赖 window.aceDesktop_notify 桥,
// 通过最小 stub 注入测。
//
// Windows 与 macOS native 后端共享同一套前端行为。

import assert from 'node:assert/strict';
import {
  buildNotificationPayload,
  notificationBodyFromEvent,
  shouldNotifySessionCompletion,
  shouldSuppress,
  truncateForNotification,
  maybeNotify,
  noteHostWindowFocus,
  isHostWindowFocused,
} from './desktopNotify.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('truncateForNotification 短文本不变', () => {
  assert.equal(truncateForNotification('hello'), 'hello');
  assert.equal(truncateForNotification(''), '');
});

run('truncateForNotification 超长按 codepoint 截断 + 省略号', () => {
  const long = 'a'.repeat(120);
  const out = truncateForNotification(long);
  assert.equal(Array.from(out).length, 81); // 80 + …
  assert.ok(out.endsWith('…'));
});

run('truncateForNotification 中文 emoji 按 codepoint 不破坏字符', () => {
  // 100 个中文,limit 80
  const cn = '一'.repeat(100);
  const out = truncateForNotification(cn);
  assert.equal(Array.from(out).length, 81);
  assert.ok(out.endsWith('…'));
  // emoji surrogate 不被截到一半
  const emoji = '🚀'.repeat(50);
  const out2 = truncateForNotification(emoji, 5);
  assert.equal(Array.from(out2).length, 6); // 5 emoji + …
});

run('buildNotificationPayload question 类型基础形态', () => {
  const p = buildNotificationPayload({
    type: 'question',
    sessionId: 's1',
    workspaceHash: 'w1',
    sessionTitle: '我的会话',
    bodyText: '请确认是否继续',
  });
  assert.equal(p.title, '需要你回答 · 我的会话');
  assert.equal(p.body, '请确认是否继续');
  assert.equal(p.session_id, 's1');
  assert.equal(p.workspace_hash, 'w1');
  assert.match(p.id, /^question-s1-/);
});

run('buildNotificationPayload completion 默认 title suffix 是 "会话"', () => {
  const p = buildNotificationPayload({
    type: 'completion',
    sessionId: 's2',
    bodyText: '任务完成',
  });
  assert.equal(p.title, '已完成 · 会话');
  assert.match(p.id, /^completion-s2-/);
});

run('buildNotificationPayload permission 使用授权标题和稳定类型', () => {
  const p = buildNotificationPayload({
    type: 'permission',
    sessionId: 's-perm',
    sessionTitle: '后台会话',
    bodyText: '等待权限',
  });
  assert.equal(p.title, '需要你授权 · 后台会话');
  assert.match(p.id, /^permission-s-perm-/);
});

run('notificationBodyFromEvent 提取权限工具和首个问题', () => {
  assert.equal(
    notificationBodyFromEvent('permission', { tool: 'write_file' }),
    '工具 write_file 正在等待权限确认',
  );
  assert.equal(
    notificationBodyFromEvent('question', {
      questions: [{ question: '要继续吗？' }],
    }),
    '要继续吗？',
  );
});

run('buildNotificationPayload 空 body 时 completion 走默认占位', () => {
  const p = buildNotificationPayload({ type: 'completion', sessionId: 's3', bodyText: '' });
  assert.equal(p.body, '(空白回合)');
});

run('buildNotificationPayload 空 body 时 question 留空 body', () => {
  const p = buildNotificationPayload({ type: 'question', sessionId: 's4', bodyText: '   ' });
  assert.equal(p.body, '');
});

run('buildNotificationPayload 长 body 被截断', () => {
  const p = buildNotificationPayload({
    type: 'question', sessionId: 's5', bodyText: '一'.repeat(200),
  });
  assert.equal(Array.from(p.body).length, 81);
});

run('buildNotificationPayload 未知 type 当 question', () => {
  const p = buildNotificationPayload({ type: 'weird', sessionId: 's6' });
  assert.match(p.id, /^question-/);
  assert.match(p.title, /^需要你回答/);
});

run('shouldNotifySessionCompletion 主会话与未知归属保持可通知', () => {
  assert.equal(shouldNotifySessionCompletion({
    sessionId: 'main-1',
    ownerSessionId: 'main-1',
  }), true);
  assert.equal(shouldNotifySessionCompletion({ sessionId: 'main-2' }), true);
});

run('shouldNotifySessionCompletion 显式 parent_session_id 屏蔽子代理完成通知', () => {
  assert.equal(shouldNotifySessionCompletion({
    sessionId: 'child-1',
    parentSessionId: 'parent-1',
  }), false);
});

run('shouldNotifySessionCompletion 已登记 owner 屏蔽子代理完成通知', () => {
  assert.equal(shouldNotifySessionCompletion({
    sessionId: 'child-2',
    ownerSessionId: 'parent-2',
  }), false);
});

run('shouldNotifySessionCompletion 缺失 session id 不可通知', () => {
  assert.equal(shouldNotifySessionCompletion({ ownerSessionId: 'parent-3' }), false);
});

// shouldSuppress 抑制规则
const sampleQuestion = {
  id: 'question-s1-123',
  session_id: 's1',
  workspace_hash: 'w1',
  title: 't', body: 'b',
};
const sampleCompletion = {
  id: 'completion-s2-123',
  session_id: 's2',
  workspace_hash: 'w1',
  title: 't', body: 'b',
};
const samplePermission = {
  id: 'permission-s3-123',
  session_id: 's3',
  workspace_hash: 'w1',
  title: 't', body: 'b',
};

run('shouldSuppress: enabled=false 一律抑制', () => {
  assert.equal(shouldSuppress(sampleQuestion, false, { enabled: false }), true);
  assert.equal(shouldSuppress(sampleCompletion, false, { enabled: false }), true);
});

run('shouldSuppress: permission 和 question 在窗口失焦时也不弹', () => {
  assert.equal(shouldSuppress(samplePermission, false, null), true);
  assert.equal(shouldSuppress(sampleQuestion, false, null), true);
});

run('shouldSuppress: 主任务完成且窗口失焦时允许弹窗', () => {
  assert.equal(shouldSuppress(sampleCompletion, false, null), false);
});

run('shouldSuppress: 窗口聚焦时一律抑制完成弹窗', () => {
  assert.equal(shouldSuppress(sampleCompletion, true, null), true);
});

run('shouldSuppress: 旧分项字段不能改变固定规则', () => {
  const legacyCfg = {
    on_permission: true,
    on_question: true,
    on_completion: false,
    suppress_when_focused: false,
  };
  assert.equal(shouldSuppress(samplePermission, false, legacyCfg), true);
  assert.equal(shouldSuppress(sampleQuestion, false, legacyCfg), true);
  assert.equal(shouldSuppress(sampleCompletion, false, legacyCfg), false);
  assert.equal(shouldSuppress(sampleCompletion, true, legacyCfg), true);
});

run('isHostWindowFocused: visibility=hidden 一律视为未聚焦', () => {
  noteHostWindowFocus(true);
  assert.equal(
    isHostWindowFocused({ visibilityState: 'hidden', hasFocus: () => true }),
    false,
  );
});

run('isHostWindowFocused: hasFocus=true 优先于 sticky blur', () => {
  noteHostWindowFocus(false);
  assert.equal(
    isHostWindowFocused({ visibilityState: 'visible', hasFocus: () => true }),
    true,
  );
});

run('isHostWindowFocused: hasFocus=false 时回退 sticky focus 标志', () => {
  noteHostWindowFocus(true);
  assert.equal(
    isHostWindowFocused({ visibilityState: 'visible', hasFocus: () => false }),
    true,
  );
  noteHostWindowFocus(false);
  assert.equal(
    isHostWindowFocused({ visibilityState: 'visible', hasFocus: () => false }),
    false,
  );
});

// maybeNotify 在无桥时静默 no-op
run('maybeNotify 无 desktop 桥时返回 false 不抛错', () => {
  // 模拟浏览器直访模式:window 无 aceDesktop_notify
  const prev = global.window;
  global.window = {};
  const ok = maybeNotify({
    type: 'question',
    sessionId: 's1',
    bodyText: 'hello',
    hasFocus: false,
    cfg: null,
  });
  assert.equal(ok, false);
  global.window = prev;
});

run('maybeNotify 桥可用 + 抑制规则不命中 → 投递', () => {
  const prev = global.window;
  let captured = null;
  global.window = {
    aceDesktop_notify: (json) => { captured = JSON.parse(json); },
  };
  const ok = maybeNotify({
    type: 'completion',
    sessionId: 's-x',
    workspaceHash: 'w-x',
    sessionTitle: 'Test',
    bodyText: '完工了',
    hasFocus: false,
    cfg: null,
  });
  assert.equal(ok, true);
  assert.equal(captured.session_id, 's-x');
  assert.equal(captured.title, '已完成 · Test');
  assert.equal(captured.body, '完工了');
  global.window = prev;
});

run('maybeNotify 桥可用 + 窗口聚焦 → 不投递完成通知', () => {
  const prev = global.window;
  let called = 0;
  global.window = {
    aceDesktop_notify: () => { called += 1; },
  };
  const ok = maybeNotify({
    type: 'completion',
    sessionId: 's1',
    workspaceHash: 'w1',
    bodyText: 'done',
    hasFocus: true,
    cfg: null,
  });
  assert.equal(ok, false);
  assert.equal(called, 0);
  global.window = prev;
});
