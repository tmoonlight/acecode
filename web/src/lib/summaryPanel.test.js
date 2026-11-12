import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import React from 'react';
import { renderToStaticMarkup } from 'react-dom/server';
import { parseSync } from '@babel/core';
import { transformWithEsbuild } from 'vite';
import { clsx } from './format.js';
import {
  folderLabel,
  formatChangeCount,
  isEditableWorkspaceHash,
  summaryChangeTotals,
  summaryFinishedTaskCount,
  summaryRunningTasks,
  summaryWorkspaceModel,
} from './summaryPanel.js';
import { normalizeSessionOutputs, sessionOutputRows } from './sessionOutputs.js';
import {
  publishWorkspaceDirectory,
  subscribeWorkspaceDirectory,
  workspaceDirectoryEntry,
} from './workspaceDirectory.js';

function run(name, test) { test(); console.log(`[pass] ${name}`); }

run('工作区行:齿轮只给侧栏能编辑的工作区,分支只在 git 仓库里显示', () => {
  // 场景:普通注册工作区 + git 仓库 → 有名称、路径、齿轮和分支。
  const repo = summaryWorkspaceModel({
    name: ' acecode ', cwd: 'N:/Users/shao/acecode', hash: 'abc123',
    gitInfo: { is_repo: true, branch: 'master' },
  });
  assert.deepEqual(repo, {
    name: 'acecode', folders: ['N:/Users/shao/acecode'], icon: null, editable: true, hash: 'abc123', branch: 'master',
  });
  // 场景:本机兜底项与无工作区会话 → 侧栏没有「编辑」,这里也不能出现齿轮。
  assert.equal(isEditableWorkspaceHash('__local__'), false);
  assert.equal(isEditableWorkspaceHash(''), false);
  const none = summaryWorkspaceModel({ name: 'x', cwd: 'C:/tmp', hash: 'abc123', noWorkspace: true });
  assert.equal(none.editable, false);
  assert.equal(none.name, '');
  // 场景:不是 git 仓库 → 不显示分支行(gitInfo 里残留的 branch 也不采用)。
  assert.equal(summaryWorkspaceModel({ gitInfo: { is_repo: false, branch: 'main' } }).branch, '');
});

run('变更合计:仓库用工作树相对基线的总数,非仓库退到本会话改动,没改动整段隐藏', () => {
  // 场景:git 仓库且有改动 → 取 /api/git/changes 的合计。
  assert.deepEqual(
    summaryChangeTotals({ isRepo: true, gitList: { total_additions: 4572, total_deletions: 623, total_count: 12 } }),
    { additions: 4572, deletions: 623 },
  );
  // 场景:仓库数据还没拉到 / 工作树干净 → null,不显示「+0 −0」这种空行。
  assert.equal(summaryChangeTotals({ isRepo: true, gitList: null }), null);
  assert.equal(summaryChangeTotals({ isRepo: true, gitList: { total_additions: 0, total_deletions: 0, total_count: 0 } }), null);
  // 场景:只有新增的未跟踪文件(无行数)也算有变更,行数显示 0。
  assert.deepEqual(summaryChangeTotals({ isRepo: true, gitList: { files: [{ path: 'a', status: 'A' }] } }),
    { additions: 0, deletions: 0 });
  // 场景:非 git 工作区 → 用本会话的改动汇总;会话没改过文件则隐藏。
  assert.deepEqual(summaryChangeTotals({ sessionSummary: { hasChanges: true, totalAdditions: 3, totalDeletions: 1 } }),
    { additions: 3, deletions: 1 });
  assert.equal(summaryChangeTotals({ sessionSummary: { hasChanges: false } }), null);
  // 千分位与 Claude 桌面版一致:+4,572。
  assert.equal(formatChangeCount(4572), '4,572');
  assert.equal(formatChangeCount(-5), '0');
});

run('后台任务行:只列运行中的,先开始的在上,停止请求发出后标记 stopping', () => {
  const now = 100_000;
  const rows = summaryRunningTasks([
    { id: 'late', status: 'running', title: '后启动', createdAtMs: now - 6_000 },
    { id: 'done', status: 'completed', title: '已结束', createdAtMs: now - 90_000, updatedAtMs: now - 1_000 },
    { id: 'early', status: 'running', title: '[Error] auto title failed', summary: '先启动', createdAtMs: now - 24_000, abortPending: true },
  ], now);
  // 已结束的任务不进摘要;顺序按开始时间升序(与 Claude 桌面版一致)。
  assert.deepEqual(rows.map((row) => row.id), ['early', 'late']);
  // 自动标题生成失败的占位退到首条输入摘要。
  assert.equal(rows[0].title, '先启动');
  assert.equal(rows[0].elapsed, '24s');
  assert.equal(rows[0].stopping, true);
  assert.equal(rows[1].elapsed, '06s');
  assert.equal(rows[1].stopping, false);
  assert.deepEqual(summaryRunningTasks(null), []);
});

// 场景:摘要面板的子代理行前面画头像(绑定了专家用专家的,否则按子会话 id 生成)。
// 期望:子代理行带上任务的专家信息;后台命令行没有头像,不带专家信息。
run('后台任务行:子代理带专家信息给头像用,后台命令不带', () => {
  const expert = { avatar_identity: 'expert:reviewer', avatar_url: '' };
  const rows = summaryRunningTasks([
    { id: 'agent-with-expert', status: 'running', title: '审查', createdAtMs: 1_000, expert },
    { id: 'agent-plain', status: 'running', title: '调研', createdAtMs: 2_000 },
    { id: 'bg_1', kind: 'command', status: 'running', command: 'pnpm build', createdAtMs: 3_000, expert },
  ], 10_000);
  assert.deepEqual(rows.map((row) => [row.id, row.kind, row.expert]), [
    ['agent-with-expert', 'agent', expert],
    ['agent-plain', 'agent', null],
    ['bg_1', 'command', null],
  ]);
});

// 场景:摘要面板只在有任务时显示「后台任务」,有已结束任务才显示「N 已完成」。
// 期望:已完成数 = 不在运行中的任务(完成、停止都算),任务每结束一个数字加一;没有任务时为 0。
run('后台任务已完成计数:结束一个加一,运行中的不算', () => {
  const tasks = [
    { id: 'a', status: 'running' },
    { id: 'b', status: 'completed' },
    { id: 'c', status: 'aborted' },
  ];
  assert.equal(summaryFinishedTaskCount(tasks), 2);
  const afterOneMore = tasks.map((task) => (task.id === 'a' ? { ...task, status: 'completed' } : task));
  assert.equal(summaryFinishedTaskCount(afterOneMore), 3);
  assert.equal(summaryFinishedTaskCount([]), 0);
  assert.equal(summaryFinishedTaskCount(null), 0);
});

// 渲染生产组件,只替换取数和计时 hook,覆盖实际 JSX 的分组与完成行显示条件。
const panelSource = readFileSync(new URL('../components/SummaryPanel.jsx', import.meta.url), 'utf8');
const panelAst = parseSync(panelSource, { configFile: false, babelrc: false, parserOpts: { plugins: ['jsx'] } });
const panelComponents = ['Section', 'Row', 'IconButton', 'OutputRow', 'OutputsSection', 'SummaryPanel'];
// 产物列表由 useSessionOutputs 取数,这里换成直接返回下面这个变量。
let panelOutputs = [];
const declarations = panelAst.program.body.map((node) => node.declaration || node)
  .filter((node) => node.type === 'FunctionDeclaration' && panelComponents.includes(node.id.name));
assert.equal(declarations.length, panelComponents.length);
const compiledPanel = await transformWithEsbuild(
  declarations.map((node) => panelSource.slice(node.start, node.end)).join('\n'),
  'SummaryPanel.jsx',
  { loader: 'jsx', jsxFactory: 'React.createElement', jsxFragment: 'React.Fragment' },
);
const SummaryPanel = vm.runInNewContext(`${compiledPanel.code}; SummaryPanel;`, {
  React, ...React, clsx,
  summaryWorkspaceModel, summaryChangeTotals, summaryRunningTasks, summaryFinishedTaskCount, formatChangeCount,
  useFolderContextMenu() {},
  useWorkspaceDirectoryEntry: () => null,
  useGitInfo: () => null,
  useGitChangeList: () => null,
  useNow: () => 100_000,
  useSessionOutputs: () => panelOutputs,
  sessionOutputRows,
  SUMMARY_FOLDER_CONTEXT_MENU_DELEGATE: 'summary-folder',
  SUMMARY_ROW_BUTTON_CLASS: 'row-button',
  FileTypeIcon: ({ path }) => React.createElement('span', { 'data-file-icon': path }),
  VsIcon: ({ name }) => React.createElement('span', { 'data-icon': name }),
  SubagentAvatar: ({ sessionId }) => React.createElement('span', { 'data-avatar': sessionId }),
});
const renderPanel = (tasks, props = {}) => renderToStaticMarkup(React.createElement(SummaryPanel, {
  tasks, workspaceName: '测试工作区', ...props,
}));
const dividerCount = (html) => (html.match(/h-px bg-border mx-2/g) || []).length;

run('后台任务摘要:没有任务时隐藏整个分组及分隔线', () => {
  for (const tasks of [undefined, null, [], [null]]) {
    const html = renderPanel(tasks);
    assert.match(html, /测试工作区/);
    assert.doesNotMatch(html, /后台任务|已完成/);
    assert.equal(dividerCount(html), 0);
  }
  const withChanges = renderPanel([], {
    sessionChangeSummary: { hasChanges: true, totalAdditions: 3, totalDeletions: 1 },
  });
  assert.match(withChanges, /变更/);
  assert.doesNotMatch(withChanges, /后台任务|已完成/);
  assert.equal(dividerCount(withChanges), 1);
});

run('后台任务摘要:只有运行中的后台命令或子代理时不显示零完成行', () => {
  for (const kind of ['command', 'agent']) {
    const html = renderPanel([{ id: kind, kind, status: 'running', title: '正在处理', createdAtMs: 94_000 }]);
    assert.match(html, /后台任务/);
    assert.match(html, /正在处理/);
    assert.match(html, /data-icon="stopFilled"/);
    assert.doesNotMatch(html, /已完成|data-icon="check"/);
    assert.equal(dividerCount(html), 1);
  }
});

// 场景:摘要面板「后台任务」里有运行中的子代理,也有运行中的后台命令。
// 期望:子代理行前面是它的头像(不再是通用的智能体图标),后台命令行仍是终端图标。
run('后台任务摘要:子代理行画头像,后台命令行保持终端图标', () => {
  const agentHtml = renderPanel([{ id: 'child-1', status: 'running', title: '调研', createdAtMs: 94_000 }]);
  assert.match(agentHtml, /data-avatar="child-1"/);
  assert.doesNotMatch(agentHtml, /data-icon="(?:embedding|terminal)"/);
  const commandHtml = renderPanel([
    { id: 'bg_1', kind: 'command', status: 'running', command: 'pnpm build', createdAtMs: 94_000 },
  ]);
  assert.match(commandHtml, /data-icon="terminal"/);
  assert.doesNotMatch(commandHtml, /data-avatar=/);
});

run('后台任务摘要:有已结束任务时保留分组和正确完成数量', () => {
  const settled = [{ id: 'done', status: 'completed' }, { id: 'stopped', status: 'aborted' }];
  for (const tasks of [settled, [...settled, { id: 'running', status: 'running', title: '仍在处理' }]]) {
    const html = renderPanel(tasks);
    assert.match(html, /后台任务/);
    assert.match(html, /2 已完成/);
    assert.equal(dividerCount(html), 1);
    assert.equal(html.includes('仍在处理'), tasks.length === 3);
  }
});

const withOutputs = (count, render) => {
  panelOutputs = normalizeSessionOutputs({
    outputs: Array.from({ length: count }, (_, index) => ({
      path: `/w/docs/file${index}.md`, relative: `docs/file${index}.md`, modified_ms: 1000 - index,
    })),
  });
  try { return render(); } finally { panelOutputs = []; }
};

// 场景:这次对话还没有生成任何文件。
// 期望:整个「产物」分组和它的分隔线都不画(不留一个空标题)。
run('产物分组:没有产物时不显示分组和分隔线', () => {
  const html = renderPanel([]);
  assert.doesNotMatch(html, /产物|显示更多/);
  assert.equal(dividerCount(html), 0);
});

// 场景:对话生成了 2 个文档。
// 期望:「产物」分组排在工作区后面、变更前面;每个文件一行,带文件类型图标、
// 完整路径在 title 里;行上有右键菜单的委托标记(kind=file);不到 6 个不出现「显示更多」。
run('产物分组:每个文件一行,排在工作区和变更之间', () => {
  const html = withOutputs(2, () => renderPanel([], {
    sessionId: 's1',
    onOpenFile() {},
    sessionChangeSummary: { hasChanges: true, totalAdditions: 3, totalDeletions: 1 },
  }));
  assert.ok(html.indexOf('测试工作区') < html.indexOf('产物'));
  assert.ok(html.indexOf('产物') < html.indexOf('变更'));
  assert.match(html, /data-file-icon="file0\.md"/);
  assert.match(html, /title="\/w\/docs\/file1\.md"/);
  assert.match(html, /data-summary-path-kind="file"/);
  assert.match(html, /data-summary-folder-path="\/w\/docs\/file0\.md"/);
  assert.doesNotMatch(html, /显示更多/);
  assert.equal(dividerCount(html), 2);
});

// 场景:对话生成了 8 个文件。
// 期望:先只显示最近的 5 个,下面有「显示更多」(第 6 个起不在页面里)。
run('产物分组:超过 5 个时先显示前 5 个并给出「显示更多」', () => {
  const html = withOutputs(8, () => renderPanel([], { sessionId: 's1', onOpenFile() {} }));
  assert.match(html, /file4\.md/);
  assert.doesNotMatch(html, /file5\.md/);
  assert.match(html, /显示更多/);
});

// Windows 路径用反斜杠拼接(源码里不直接写反斜杠字面量)。
const winPath = (...parts) => parts.join(String.fromCharCode(92));

run('工作区行:自定义图标/颜色与附加文件夹取自侧栏已加载的工作区条目', () => {
  const directory = {
    hash: 'abc123', name: 'ttt', icon: { id: 'music', color: 'blue' },
    extra_folders: ['D:/shared/lib', winPath('c:', 'users', 'shao', 'ttt', ''), 'D:/shared/lib/'],
  };
  const model = summaryWorkspaceModel({
    name: '旧名字', cwd: winPath('C:', 'Users', 'shao', 'ttt'), hash: 'abc123', directory,
  });
  // 场景:侧栏里改过名 → 用侧栏的新名字,而不是会话引用里残留的旧名。
  assert.equal(model.name, 'ttt');
  // 场景:设置了自定义图标 → 第一行用它(图标 id + 颜色)。
  assert.deepEqual(model.icon, { id: 'music', color: 'blue' });
  // 场景:多个目录 → 会话目录在前,附加文件夹随后;同一路径(大小写 / 斜杠 / 结尾分隔符
  // 不同)只列一次。
  assert.deepEqual(model.folders, [winPath('C:', 'Users', 'shao', 'ttt'), 'D:/shared/lib']);
  // 场景:图标是默认文件夹 + 默认色,等价于没设置 → 不当作自定义图标。
  assert.equal(summaryWorkspaceModel({ directory: { icon: { id: 'folder', color: 'default' } } }).icon, null);
  // 场景:无工作区会话即使误带了条目,也不显示名称、图标和附加文件夹。
  const none = summaryWorkspaceModel({ cwd: 'C:/tmp', noWorkspace: true, directory });
  assert.equal(none.icon, null);
  assert.deepEqual(none.folders, ['C:/tmp']);
});

run('共享工作区列表:同一批对象重复发布不通知,内容变化才通知', () => {
  let notified = 0;
  const unsubscribe = subscribeWorkspaceDirectory(() => { notified += 1; });
  const ttt = { hash: 'h1', name: 'ttt' };
  assert.equal(publishWorkspaceDirectory([ttt, { name: '没有 hash 的条目被忽略' }]), true);
  assert.equal(workspaceDirectoryEntry('h1'), ttt);
  // 侧栏刷新但列表没变(同一组对象引用)→ 不打扰订阅方。
  assert.equal(publishWorkspaceDirectory([ttt]), false);
  // 改名后侧栏换了新对象 → 通知。
  assert.equal(publishWorkspaceDirectory([{ ...ttt, name: 'renamed' }]), true);
  assert.equal(workspaceDirectoryEntry('h1').name, 'renamed');
  assert.equal(notified, 2);
  unsubscribe();
  publishWorkspaceDirectory([]);
  assert.equal(notified, 2);
  assert.equal(workspaceDirectoryEntry('h1'), null);
});

run('目录行文案:「目录名 · 上级路径」,正反斜杠与结尾分隔符都能处理', () => {
  // 场景:普通 Windows 路径 → 名字是最后一级,上级保留原样分隔符。
  assert.deepEqual(folderLabel(winPath('N:', 'Users', 'shao', 'acecode')), { name: 'acecode', parent: winPath('N:', 'Users', 'shao') });
  // 场景:正斜杠 + 结尾分隔符 → 结尾分隔符不算一级。
  assert.deepEqual(folderLabel('N:/Users/shao/test333/'), { name: 'test333', parent: 'N:/Users/shao' });
  // 场景:盘符下一级 → 上级写成 L:/(不显示成光秃秃的 L:)。
  assert.deepEqual(folderLabel('L:/u3d'), { name: 'u3d', parent: 'L:/' });
  // 场景:POSIX 路径。
  assert.deepEqual(folderLabel('/home/allan/ws'), { name: 'ws', parent: '/home/allan' });
  // 场景:盘符根 / 没有分隔符 / 空串 → 整体当名字,不显示上级。
  assert.deepEqual(folderLabel('C:/'), { name: 'C:/', parent: '' });
  assert.deepEqual(folderLabel('scratch'), { name: 'scratch', parent: '' });
  assert.deepEqual(folderLabel(''), { name: '', parent: '' });
});
