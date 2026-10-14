import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import {
  TURN_FILE_LIST_COLLAPSE_THRESHOLD,
  buildTurnFileItems,
  splitTurnFileItems,
  turnFileDisplayPath,
  turnFileOpenPath,
} from './turnFileList.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('turnFileDisplayPath cwd 内绝对路径相对化,cwd 外保持绝对', () => {
  // 场景:worktree 会话里工具用绝对路径落盘(hunks 的 file 是
  // N:/Users/.../ses-xxx/random_fun_1.txt),旧 UI 会把 name 和整段父目录
  // 各显示一遍。期望:cwd 内 → 只剩相对路径;cwd 外 → 原样绝对路径。
  const cwd = 'N:/Users/shao/ttt/.acecode/worktrees/ses-1';
  assert.equal(
    turnFileDisplayPath('N:/Users/shao/ttt/.acecode/worktrees/ses-1/random_fun_1.txt', cwd),
    'random_fun_1.txt',
  );
  assert.equal(
    turnFileDisplayPath('N:/Users/shao/ttt/.acecode/worktrees/ses-1/sub/a.js', cwd),
    'sub/a.js',
  );
  assert.equal(
    turnFileDisplayPath('N:/Users/shao/other/b.js', cwd),
    'N:/Users/shao/other/b.js',
  );
});

run('turnFileDisplayPath 反斜杠归一化 + Windows 盘符大小写不敏感', () => {
  // 场景:cwd 来自 daemon(反斜杠 + 盘符大写),file 来自工具 canonical 化
  // (正斜杠 + 盘符可能变小写,参见 memory: C:\Users\shao 是 N: 的 junction)。
  // 期望:两种形态照样匹配、相对化;POSIX 路径(无盘符)保持大小写敏感。
  assert.equal(
    turnFileDisplayPath('n:/users/Shao/proj/src/a.cpp', 'N:\\Users\\shao\\proj'),
    'src/a.cpp',
  );
  assert.equal(
    turnFileDisplayPath('/home/User/proj/a.c', '/home/user/proj'),
    '/home/User/proj/a.c',
  );
});

run('turnFileDisplayPath 相对路径与空 cwd 原样透传', () => {
  // 场景:非 worktree 会话里 file_edit 常给相对路径;cwd 未知(健康检查
  // 未返回)时也不能崩。期望:原样返回(仅斜杠归一化)。
  assert.equal(turnFileDisplayPath('src/foo.js', 'N:/Users/shao/proj'), 'src/foo.js');
  assert.equal(turnFileDisplayPath('src\\foo.js', ''), 'src/foo.js');
  assert.equal(turnFileDisplayPath('', 'N:/x'), '');
});

run('buildTurnFileItems 透传加删行数并生成展示路径', () => {
  // 场景:行尾要跟红绿 +xx -xx,数据来自 group 的 totalAdditions/Deletions。
  // 期望:数字缺失/非法时归零(finiteNumber 防御),displayPath 相对化。
  const items = buildTurnFileItems([
    { file: 'N:/proj/a.js', totalAdditions: 12, totalDeletions: 3 },
    { file: 'b.js', totalAdditions: null, totalDeletions: NaN },
  ], 'N:/proj');
  assert.deepEqual(items, [
    { file: 'N:/proj/a.js', openPath: 'N:/proj/a.js', displayPath: 'a.js', additions: 12, deletions: 3 },
    { file: 'b.js', openPath: 'b.js', displayPath: 'b.js', additions: 0, deletions: 0 },
  ]);
});

run('turnFileOpenPath 剥掉旧版 worktree 净差异里重复的 worktree 前缀', () => {
  // 场景:旧版回合净差异在 worktree 会话里按会话原目录相对化,记成
  // ".acecode/worktrees/ses-1/src/a.cpp";会话 cwd 是 worktree 本身。
  // 回归:直接拼接得到 ".../worktrees/ses-1/.acecode/worktrees/ses-1/src/a.cpp",
  // 回合末尾文件卡片点「打开」报文件找不到,「查看变更」却正常。
  // 期望:剥掉与当前 worktree 重复的前缀,只剩 worktree 内的相对路径。
  const cwd = 'N:/Users/shao/proj/.acecode/worktrees/ses-1';
  assert.equal(turnFileOpenPath('.acecode/worktrees/ses-1/src/a.cpp', cwd), 'src/a.cpp');
  assert.equal(turnFileOpenPath('.acecode\\worktrees\\ses-1\\b.txt', 'N:\\Users\\shao\\proj\\.acecode\\worktrees\\ses-1\\'), 'b.txt');
  // 盘符路径大小写不敏感
  assert.equal(turnFileOpenPath('.ACECODE/Worktrees/SES-1/c.js', cwd), 'c.js');
});

run('turnFileOpenPath 其它路径原样返回', () => {
  // 场景:新版净差异在 worktree 里是绝对路径;普通会话是相对路径;别的
  // worktree 的前缀不能误剥。期望:都不改动。
  const cwd = 'N:/Users/shao/proj/.acecode/worktrees/ses-1';
  assert.equal(
    turnFileOpenPath('N:/Users/shao/proj/.acecode/worktrees/ses-1/src/a.cpp', cwd),
    'N:/Users/shao/proj/.acecode/worktrees/ses-1/src/a.cpp',
  );
  assert.equal(turnFileOpenPath('.acecode/worktrees/ses-2/a.cpp', cwd), '.acecode/worktrees/ses-2/a.cpp');
  assert.equal(turnFileOpenPath('.acecode/worktrees/ses-1/a.cpp', 'N:/Users/shao/proj'), '.acecode/worktrees/ses-1/a.cpp');
  assert.equal(turnFileOpenPath('src/a.cpp', 'N:/Users/shao/proj'), 'src/a.cpp');
  assert.equal(turnFileOpenPath('', cwd), '');
});

run('buildTurnFileItems 旧版 worktree 记录:展示与打开用修正路径,file 保留原始 key', () => {
  // 场景:worktree 会话的旧记录。期望:displayPath / openPath 都是 worktree 内
  // 的相对路径;file 保持原值,「查看变更」靠它匹配 diff 组。
  const cwd = 'N:/Users/shao/proj/.acecode/worktrees/ses-1';
  const [item] = buildTurnFileItems([
    { file: '.acecode/worktrees/ses-1/src/a.cpp', totalAdditions: 1, totalDeletions: 0 },
  ], cwd);
  assert.equal(item.file, '.acecode/worktrees/ses-1/src/a.cpp');
  assert.equal(item.openPath, 'src/a.cpp');
  assert.equal(item.displayPath, 'src/a.cpp');
});

run('buildTurnFileItems 跳过非法条目,非数组输入返回空数组', () => {
  // 场景:上游聚合可能混入 file 为空/缺失的脏数据;整个 groups 也可能是 null
  //(会话切换的瞬时帧)。期望:不抛异常,静默过滤。
  assert.deepEqual(buildTurnFileItems(null), []);
  assert.deepEqual(
    buildTurnFileItems([{ file: '' }, null, { notFile: 1 }, { file: 'a.js' }]).map((i) => i.file),
    ['a.js'],
  );
});

run('splitTurnFileItems 条目数不超过阈值时不折叠', () => {
  // 场景:本轮只改了 3 个文件(等于阈值)。期望:全部可见、无隐藏计数、
  // 不具备折叠能力(UI 不显示「展开查看剩余」按钮)。
  const items = buildTurnFileItems([
    { file: 'a.js' }, { file: 'b.js' }, { file: 'c.js' },
  ]);
  const { visible, hiddenCount, collapsible } = splitTurnFileItems(items, false);
  assert.equal(visible.length, 3);
  assert.equal(hiddenCount, 0);
  assert.equal(collapsible, false);
});

run('splitTurnFileItems 超过阈值且未展开时只露前 3 个并给出剩余数', () => {
  // 场景:本轮改了 5 个文件,用户尚未点「展开」。期望:可见前 3 个
  //(保持原顺序 = 文件首次被改动的顺序),hiddenCount = 2,collapsible = true,
  // 对应 UI 文案「展开查看剩余 2 个文件」。
  const items = buildTurnFileItems(
    ['a.js', 'b.js', 'c.js', 'd.js', 'e.js'].map((file) => ({ file })),
  );
  const { visible, hiddenCount, collapsible } = splitTurnFileItems(items, false);
  assert.deepEqual(visible.map((i) => i.displayPath), ['a.js', 'b.js', 'c.js']);
  assert.equal(hiddenCount, 2);
  assert.equal(collapsible, true);
});

run('splitTurnFileItems 展开后全部可见,collapsible 仍为 true 供「收起」按钮使用', () => {
  // 场景:5 个文件,用户已点「展开查看剩余 2 个文件」。期望:5 个全可见、
  // hiddenCount 归零;collapsible 保持 true,UI 据此渲染「收起」。
  const items = buildTurnFileItems(
    ['a.js', 'b.js', 'c.js', 'd.js', 'e.js'].map((file) => ({ file })),
  );
  const { visible, hiddenCount, collapsible } = splitTurnFileItems(items, true);
  assert.equal(visible.length, 5);
  assert.equal(hiddenCount, 0);
  assert.equal(collapsible, true);
});

run('splitTurnFileItems 非法阈值回退默认值', () => {
  // 场景:调用方误传 0 / NaN / 负数阈值。期望:回退默认阈值 3,
  // 而不是把列表切空或抛异常(防御性,阈值只该来自常量)。
  const items = buildTurnFileItems(
    ['a.js', 'b.js', 'c.js', 'd.js'].map((file) => ({ file })),
  );
  for (const bad of [0, -1, NaN, undefined]) {
    const { visible, hiddenCount } = splitTurnFileItems(items, false, bad);
    assert.equal(visible.length, TURN_FILE_LIST_COLLAPSE_THRESHOLD);
    assert.equal(hiddenCount, 1);
  }
});

run('轮次文件入口把稳定 user message UUID 贯穿列表、页签和详情文件切换', () => {
  const turnFileList = readFileSync(new URL('../components/TurnFileList.jsx', import.meta.url), 'utf8');
  const chatView = readFileSync(new URL('../components/ChatView.jsx', import.meta.url), 'utf8');
  const previewDetails = readFileSync(new URL('../components/PreviewDetailsPanel.jsx', import.meta.url), 'utf8');

  assert.match(turnFileList, /onOpenChanges\?\.\(item\.file, turnUserMessageId\)/);
  assert.match(turnFileList, /onOpenFile\?\.\(item\.openPath\)/);
  const lists = [...chatView.matchAll(/<TurnFileList\b[^>]*\/>/g)];
  assert.equal(lists.length, 2);
  for (const [list] of lists) {
    assert.match(list, /onOpenChanges=\{openSessionChangePreview\}/);
    assert.match(list, /onOpenFile=\{openFilePreview\}/);
  }
  assert.equal(
    [...chatView.matchAll(/turnUserMessageId=\{set\.userMessageId\}/g)].length,
    2,
  );
  assert.match(chatView, /turnChangeSets=\{turnChangeSets\}/);
  assert.match(previewDetails, /resolveSessionChangesTabContent\(active,/);
  assert.match(
    previewDetails,
    /onSelectChangeFile\?\.\(\s*filePath,\s*scopedChanges\.turnUserMessageId,/,
  );
});
