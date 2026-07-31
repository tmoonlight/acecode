// gitSessionPill.js 的单元测试(Node + node:assert,无 DOM)。
//
// 覆盖:
//  - 非 git 仓库 / 未加载 → 不可见(零占位)
//  - 新会话可勾 worktree,但未勾时分支禁用
//  - 勾 worktree 后分支下拉才启用且语义为 worktree-base
//  - 有消息 / 已在 worktree / busy → 只读
//  - worktree 意图字段的生成条件

import assert from 'node:assert/strict';
import {
  buildPillModel,
  buildWorktreeIntent,
  shouldLoadGitInfo,
} from './gitSessionPill.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const repoInfo = {
  is_repo: true,
  branch: 'master',
  default_branch: 'master',
  branches: ['master', 'dev'],
  dirty: false,
};

run('非仓库 / 未加载 → 不可见', () => {
  assert.equal(buildPillModel({ gitInfo: null }).visible, false);
  assert.equal(buildPillModel({ gitInfo: { is_repo: false } }).visible, false);
});

run('git 仓库新会话 → 可勾 worktree,未勾时分支禁用', () => {
  const m = buildPillModel({ gitInfo: repoInfo });
  assert.equal(m.visible, true);
  assert.equal(m.branch, 'master');
  assert.deepEqual(m.branches, ['master', 'dev']);
  assert.equal(m.interactive, true);
  assert.equal(m.branchInteractive, false);
  assert.equal(m.branchSelectMeaning, 'disabled');
  assert.equal(m.started, false);
});

run('勾选 worktree → 分支下拉启用且语义变为基线选择', () => {
  const m = buildPillModel({ gitInfo: repoInfo, worktreeChecked: true });
  assert.equal(m.branchInteractive, true);
  assert.equal(m.branchSelectMeaning, 'worktree-base');
  assert.equal(m.worktreeChecked, true);
});

run('会话已有消息 → 只读徽标', () => {
  const m = buildPillModel({ gitInfo: repoInfo, sessionStarted: true });
  assert.equal(m.interactive, false);
  assert.equal(m.started, true);
});

run('已在 worktree 的会话 → 只读 + worktree 徽标 + 复选框呈勾选态', () => {
  const m = buildPillModel({
    gitInfo: repoInfo,
    worktreeSession: { name: 'ses-abc12345', branch: 'worktree-ses-abc12345' },
  });
  assert.equal(m.interactive, false);
  assert.equal(m.branchInteractive, false);
  assert.equal(m.worktreeBadge, 'ses-abc12345');
  assert.equal(m.worktreeChecked, true);
});

run('busy → worktree 与分支都暂时禁交互', () => {
  const m = buildPillModel({ gitInfo: repoInfo, worktreeChecked: true, busy: true });
  assert.equal(m.interactive, false);
  assert.equal(m.branchInteractive, false);
  assert.equal(m.branchSelectMeaning, 'disabled');
});

run('worktree 意图:勾选且未开始才生成;base 透传', () => {
  assert.equal(buildWorktreeIntent({ worktreeChecked: false }), null);
  assert.equal(
    buildWorktreeIntent({ worktreeChecked: true, sessionStarted: true }), null);
  assert.deepEqual(
    buildWorktreeIntent({ worktreeChecked: true, selectedBase: 'dev' }),
    { create: true, base: 'dev' });
  assert.deepEqual(
    buildWorktreeIntent({ worktreeChecked: true, selectedBase: '' }),
    { create: true });
});

run('Git info 只在 hero 或已加载的空白 bar 会话请求', () => {
  assert.equal(shouldLoadGitInfo({ variant: 'hero', sessionLoaded: false }), true);
  assert.equal(shouldLoadGitInfo({ variant: 'bar', sessionLoaded: false }), false);
  assert.equal(shouldLoadGitInfo({ variant: 'bar', sessionLoaded: true }), true);
  assert.equal(shouldLoadGitInfo({ variant: 'bar', sessionLoaded: true, sessionStarted: true }), false);
  assert.equal(shouldLoadGitInfo({
    variant: 'bar',
    sessionLoaded: true,
    worktreeSession: { name: 'ses-worktree' },
  }), false);
});

// 架构守护:切会话时的 /api/git/info 风暴。
//
// pill 挂在 ChatView 上、key 含 sid,每次切会话整组件重挂;而该端点在
// daemon 侧要 spawn 5~7 个 git 子进程。两条不变量必须守住:
//   1) 读取走共享缓存单例,不许退回裸 api.gitInfo;
//   2) bar 变体在会话已开始/已在 worktree 时 return null —— 这种情况下
//      必须在发请求之前就短路掉。
const pillSource = (await import('node:fs')).readFileSync(
  new URL('../components/GitSessionPill.jsx', import.meta.url),
  'utf8',
).replace(/\r\n?/g, '\n');
const chatViewSource = (await import('node:fs')).readFileSync(
  new URL('../components/ChatView.jsx', import.meta.url),
  'utf8',
).replace(/\r\n?/g, '\n');
const transcriptSource = (await import('node:fs')).readFileSync(
  new URL('./sessionTranscript.js', import.meta.url),
  'utf8',
).replace(/\r\n?/g, '\n');

run('架构: pill 读 git info 走共享缓存,且不可渲染时不发请求', () => {
  assert.match(pillSource, /import \{ gitInfoCache \} from '\.\.\/lib\/gitInfoCache\.js'/);
  assert.match(pillSource, /gitInfoCache\.get\(api, target\)/);
  assert.match(pillSource, /gitInfoCache\.invalidate\(api, target\)/);
  // 不许绕过缓存直接打接口
  assert.doesNotMatch(pillSource, /api\.gitInfo\(/);
  // 可见性短路:先算 pillCouldRender,effect 里据此提前 return
  assert.match(pillSource, /const pillCouldRender =/);
  assert.match(pillSource, /if \(!pillCouldRender\) return;/);
  // 用户显式展开下拉时强制刷新(绕过 TTL)
  assert.match(pillSource, /refreshInfo\(\{ force: true \}\)/);
  // 两个挂载点都必须保留会话绑定的 API,不能退回模块级全局客户端。
  const pillTags = chatViewSource.match(/<GitSessionPill\b[\s\S]*?\/>/g) || [];
  assert.equal(pillTags.length, 2);
  assert.ok(pillTags.every((tag) => /\bapi=\{api\}/.test(tag)));
  assert.match(pillTags[1], /sessionLoaded=\{transcriptLoadState === 'loaded'\}/);
  // sid 切换首帧不能沿用上一空会话的 loaded 状态。
  assert.match(transcriptSource, /stateSessionIdRef\.current === sid\s*\? state\.loadState/);
});

console.log('gitSessionPill.test.js: all tests passed');
