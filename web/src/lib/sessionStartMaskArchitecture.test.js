import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
}

function between(text, start, end) {
  const startIndex = text.indexOf(start);
  const endIndex = text.indexOf(end, startIndex + start.length);
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

// 触发场景:首页发送 → 建会话 → 切进新会话 → 首条消息里建 worktree。
// 期望:离开确认通过之后才开始计时(确认弹窗开着时遮罩不出现、不抢焦点);
// 会话 id 在切换界面之前记下;不管成功失败,发送流程收尾时一定结束等待。
run('首页发送全程登记等待,并在离开确认之后才开始', () => {
  const chat = source('components/ChatView.jsx');
  const create = between(chat, 'const createHomeComposerSession = useStableCallback(', '\n  const stageMediaFiles');

  assert.match(chat, /useSessionStartMask\(\{ sessionId: sid, auxiliaryPage: !!children \}\)/);
  const leave = create.indexOf('await previewLeaveRequestRef.current()');
  const begin = create.indexOf('sessionStartMask.begin({ token: sessionStart.token, home: true');
  const attach = create.indexOf('sessionStartMask.attachSession(sessionStart.token, id)');
  const promote = create.indexOf('onSessionPromoted?.(next)');
  assert.ok(leave >= 0 && begin > leave, '离开确认之后才开始等待');
  assert.ok(attach > begin && promote > attach, '切进新会话之前记下会话 id');

  const submit = between(chat, 'const submit = useStableCallback(', '\n  const drainQueuedInput');
  assert.match(submit, /sessionStart: \{ token: sessionStartToken, worktree: !!worktreeIntent \}/);
  assert.match(
    submit,
    /\.finally\(\(\) => \{\s*sessionStartMask\.end\(sessionStartToken\);\s*setComposerSubmitting\(false\);\s*restoreChatInputFocusSoon\(false\);/,
  );
  // 内置命令(/init 等)自己会跑很久,会话建好就结束等待。
  assert.match(submit, /if \(isBuiltin\) \{[^}]*sessionStartMask\.end\(sessionStartToken\);\s*await sendInputOrBuiltin/);
});

// 触发场景:已有的空会话里发首条消息并勾了 worktree。
// 期望:同样登记等待,请求结束(成功、失败、超时)时结束。
run('空会话首条消息建 worktree 也登记等待', () => {
  const chat = source('components/ChatView.jsx');
  const submit = between(chat, 'const submit = useStableCallback(', '\n  const drainQueuedInput');

  assert.match(
    submit,
    /sessionWorktreeIntent\s*\? sessionStartMask\.begin\(\{ worktree: true, sessionId: targetSid \}\)\s*: null/,
  );
  assert.match(
    submit,
    /\.finally\(\(\) => \{\s*if \(sessionStartToken\) sessionStartMask\.end\(sessionStartToken\);\s*setComposerSubmitting\(false\);\s*\}\)/,
  );
});

// 触发场景:遮罩还没到出现的时间,用户又按了发送或停止(此时 busy 只是界面先行置位)。
// 期望:入口直接早退。回归:停止会发给一个还没开始回合的会话,界面卡在「正在停止」。
run('等待期间发送与停止入口早退,不依赖遮罩是否已出现', () => {
  const chat = source('components/ChatView.jsx');

  assert.match(
    chat,
    /if \(sessionRuntimeUnavailable\) return;\s*\/\/[^\n]*\n\s*if \(sessionStartMask\.isBlocking\(\)\) return;/,
  );
  assert.match(
    between(chat, 'const stopCurrentWork = useStableCallback(', 'abort();'),
    /if \(sessionStartMask\.isBlocking\(\)\) return;/,
  );
});

// 触发场景:遮罩要同时盖住首页和会话两种界面,并且不能晚于自己的延迟再延迟一次。
// 期望:挂在 ChatView 根节点(两个分支共用),不延迟提示卡片,散掉后把焦点还给输入框。
run('遮罩挂在两个界面共用的根节点上,出现时机只由延迟门决定', () => {
  const chat = source('components/ChatView.jsx');
  const mask = source('components/SessionStartLoading.jsx');
  const hook = source('lib/useSessionStartMask.js');

  assert.match(
    chat,
    /<SessionStartLoading kind=\{sessionStartMask\.kind\} sessionId=\{sid\} \/>\s*<\/div>\s*\);\s*\}\s*$/,
  );
  assert.match(chat, /if \(wasShown && !sessionStartMaskShown\) restoreChatInputFocusSoon\(false\);/);
  assert.match(mask, /phase=\{kind \? 'starting' : ''\}/);
  assert.match(mask, /revealDelayMs=\{0\}/);
  assert.match(mask, /anchorKey=\{sessionId\}/);
  assert.match(mask, /anchorSelector="\[data-session-content-loading-anchor='true'\]"/);
  assert.match(hook, /createSlowPendingGate\(\{/);
  assert.match(source('components/SessionContentLoading.jsx'), /\[anchorKey, anchorSelector, phase\]/);
});

// 触发场景:切进新会话后有代码自动聚焦输入框,此时遮罩已经显示。
// 期望:遮罩把焦点拿回来(否则看着遮罩还能打字),但要等这次焦点事件处理完。
// 回归:在 focusin 回调里同步抢回时,Slate 先处理失焦、后处理获得焦点,一直以为自己还有焦点,
// 请求失败后遮罩散掉,输入框再也聚焦不上。
run('遮罩显示期间守住焦点,且不在焦点事件里同步抢回', () => {
  const loading = source('components/SessionContentLoading.jsx');
  const trap = between(loading, 'const onFocusIn = (event) => {', "scope.addEventListener('focusin', onFocusIn);");

  assert.match(source('components/SessionStartLoading.jsx'), /\n\s*trapFocus\n/);
  assert.match(loading, /if \(!phase \|\| !trapFocus \|\| !overlay \|\| !scope\) return undefined;/);
  assert.match(trap, /timer = window\.setTimeout\(reclaim, 0\);/);
  assert.doesNotMatch(trap, /\.focus\(/, 'focusin 回调里不能直接调用 focus()');
  assert.match(loading, /if \(timer\) window\.clearTimeout\(timer\);/);
  // 对话框盖在遮罩上面,焦点属于它。
  assert.match(loading, /closest\?\.\('\[role="dialog"\], \[aria-modal="true"\]'\)\) return;/);
});

// 触发场景:大仓库建 worktree 超过 30 秒。
// 期望:带建 worktree 意图的发送用更长的超时。回归:界面报「发送失败」,daemon 却照常建完并开始回合。
run('建 worktree 的首条消息不走默认 30 秒超时,文案保留中英文', () => {
  const api = source('lib/api.js');

  assert.match(api, /export const WORKTREE_CREATE_TIMEOUT_MS = 5 \* 60 \* 1000;/);
  assert.match(api, /body\.worktree\?\.create \? \{ timeoutMs: WORKTREE_CREATE_TIMEOUT_MS \} : \{\}/);
  for (const catalog of [source('i18n/catalogs/zh-CN.js'), source('i18n/catalogs/en-US.js')]) {
    assert.match(catalog, /sessionCreating:/);
    assert.match(catalog, /worktreeCreating:/);
  }
});
