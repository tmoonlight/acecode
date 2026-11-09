import assert from 'node:assert/strict';
import {
  folderLabel,
  formatChangeCount,
  isEditableWorkspaceHash,
  summaryChangeTotals,
  summaryFinishedTaskCount,
  summaryRunningTasks,
  summaryWorkspaceModel,
} from './summaryPanel.js';
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

// 场景:摘要面板「后台任务」分组常驻,末尾一行「N 已完成」,点开详情栏的后台任务页签。
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
