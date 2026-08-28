import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
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

// 背景:文件预览的根目录来自会话 ref 的 workingCwd。构造会话 ref 的入口不止一个,
// 且它们全是**显式字段白名单** —— 白名单里漏掉一个字段,等于在那条入口上把它删了。
//
// 触发场景:用户从侧边栏点开一个已经 active 的会话,然后点正文里的文件链接。
// 期望行为:预览打开。
// 回归背景:已 active 的会话不会重新 resume(resumeAndOpenSession 里
// shouldResume = target?.active !== true),于是 ref 只能从侧边栏传来的 target 取值。
// sidebarSessionTarget 当时没带 workingCwd,预览根目录为空,链接点了毫无反应 ——
// 而带 ?open=<id> 的入口因为会 resume、resumeResult 里有 working_cwd,反而是好的。
// 同一个功能在两条入口下表现不同,是这类白名单遗漏最典型的症状。
run('每个会话 ref 构造入口都必须带上 workingCwd', () => {
  const entries = [
    ['components/Sidebar.jsx', 'function sidebarSessionTarget'],
    ['lib/newSession.js', 'export function sessionRefFromCreateResponse'],
    ['lib/gridPinnedSessions.js', 'sessionId,'],
    ['lib/sessionJump.js', 'const copyPairs'],
  ];

  for (const [file, anchor] of entries) {
    const text = source(file);
    const start = text.indexOf(anchor);
    assert.notEqual(start, -1, `${file} 里找不到锚点 ${anchor}`);
    const body = text.slice(start, start + 3000);
    assert.match(
      body,
      /workingCwd/,
      `${file} 的会话 ref 构造漏了 workingCwd —— 这条入口进来的会话将无法预览文件`,
    );
  }
});

// 期望行为:no-workspace 会话算预览根目录时不得回退到 daemon 进程的 cwd。
// 回归背景:health.cwd 是 daemon 自己的工作目录。no-workspace 会话与它毫无关系,
// 回退过去会让预览跑到无关目录找文件并报「文件不存在」—— 实测把
// cache/no-workspace/<id>/a.xlsx 找成了 N:/Users/shao/se/a.xlsx,
// 一个看起来煞有介事的错误路径,比干脆打不开更难排查。
run('no-workspace 会话不得把 daemon 的 cwd 当预览根目录', () => {
  const chatView = source('components/ChatView.jsx');
  const start = chatView.indexOf('const sidePanelCwd = sessionWorkingCwd(');
  assert.notEqual(start, -1, '找不到 sidePanelCwd 的计算');
  const body = chatView.slice(start, start + 900);
  assert.match(
    body,
    /fallbackCwd:\s*sessionIsNoWorkspace\s*\?\s*''/,
    'no-workspace 会话必须把 fallbackCwd 置空,不能回退到 health.cwd',
  );

  const newSession = source('lib/newSession.js');
  const nsStart = newSession.indexOf('workingCwd:');
  assert.notEqual(nsStart, -1, 'newSession.js 找不到 workingCwd');
  assert.match(
    newSession.slice(nsStart, nsStart + 400),
    /noWorkspace\s*\?\s*''/,
    'newSession 的 workingCwd 对 no-workspace 会话不得回退到 health.cwd',
  );
});
