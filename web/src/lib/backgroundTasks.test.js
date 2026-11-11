import assert from 'node:assert/strict';
import {
  EMPTY_BACKGROUND_TASK_INDEX,
  applyBackgroundTaskEvent,
  backgroundTaskIdsOfItem,
  backgroundableToolStartedAt,
  commandTaskStatusLabel,
  indexBackgroundTasks,
  isBackgroundTaskFailed,
  isBackgroundTaskRunning,
  isCommandTask,
  itemHasRunningBackgroundTask,
  markCommandTaskStopping,
  mergeCommandTaskList,
  normalizeCommandTask,
  startedBackgroundTaskId,
} from './backgroundTasks.js';
import {
  backgroundTaskBody,
  backgroundTaskNotice,
  backgroundTaskNoticeCode,
  presentBackgroundTaskMessage,
} from './backgroundTaskMessage.js';
import { summaryRunningTasks } from './summaryPanel.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

function run(name, test) { test(); console.log(`[pass] ${name}`); }

const runningShell = {
  id: 'bg_1', kind: 'shell', state: 'running', command: 'npm run dev',
  started_at_ms: 1000, output_file: 'C:/tmp/bg_1.log', origin: 'requested',
};

run('后台命令归一化:只收 kind=shell,状态映射到与子代理同形的 status', () => {
  // 场景:后台子代理条目(kind=agent)由 subagentTasks 管,这里必须忽略,避免同一任务出现两次。
  assert.equal(normalizeCommandTask({ id: 'child', kind: 'agent', state: 'running' }), null);
  const task = normalizeCommandTask(runningShell);
  assert.equal(task.kind, 'command');
  assert.equal(task.status, 'running');
  assert.equal(task.createdAtMs, 1000);
  assert.equal(isCommandTask(task), true);
  // 场景:被停止 → aborted;非 0 退出码 → completed 组但标签写「失败 · 退出码」。
  assert.equal(normalizeCommandTask({ ...runningShell, state: 'stopped', ended_at_ms: 5000 }).status, 'aborted');
  const failed = normalizeCommandTask({ ...runningShell, state: 'failed', exit_code: 2, ended_at_ms: 5000 });
  assert.equal(failed.status, 'completed');
  assert.equal(commandTaskStatusLabel(failed), '失败 · 退出码 2');
});

run('事件增量:开始插入、结束替换;本地停止请求在服务端确认前保持', () => {
  let tasks = applyBackgroundTaskEvent([], runningShell);
  assert.equal(tasks.length, 1);
  tasks = markCommandTaskStopping(tasks, 'bg_1');
  assert.equal(tasks[0].stopPending, true);
  // 场景:停止请求发出后又收到一次「仍在运行」的快照 → 不能把「正在停止」清掉。
  tasks = applyBackgroundTaskEvent(tasks, { ...runningShell, last_output_at_ms: 2000 });
  assert.equal(tasks[0].stopPending, true);
  tasks = applyBackgroundTaskEvent(tasks, { ...runningShell, state: 'stopped', ended_at_ms: 3000 });
  assert.equal(tasks[0].status, 'aborted');
  assert.equal(tasks[0].stopPending, undefined);
  // 子代理事件不影响命令列表(返回同一个数组)。
  assert.equal(applyBackgroundTaskEvent(tasks, { id: 'c', kind: 'agent' }), tasks);
  // REST 快照:新任务在前。
  const merged = mergeCommandTaskList([], [runningShell, { ...runningShell, id: 'bg_2', started_at_ms: 9000 }]);
  assert.deepEqual(merged.map((task) => task.id), ['bg_2', 'bg_1']);
});

run('转到后台按钮:只认还在跑的 bash / spawn_subagent / wait_subagent,取最早开始的', () => {
  const items = [
    { kind: 'tool', tool: { tool: 'file_read', isDone: false, startedAtMs: 100 } },
    { kind: 'tool', tool: { tool: 'bash', isDone: true, startedAtMs: 200 } },
    { kind: 'tool', tool: { tool: 'spawn_subagent', isDone: false, startedAtMs: 500 } },
    { kind: 'tool', tool: { tool: 'bash', isDone: false, startedAtMs: 300 } },
  ];
  assert.equal(backgroundableToolStartedAt(items), 300);
  // 场景:只有读文件之类的快工具在跑 → 不给按钮。
  assert.equal(backgroundableToolStartedAt(items.slice(0, 2)), 0);
  assert.equal(backgroundableToolStartedAt(null), 0);
});

run('后台任务通知消息:user 角色转成系统提示行,去掉 <background-task> 外壳', () => {
  const message = {
    role: 'user',
    content: '<background-task id="bg_1" kind="command" status="failed" exit-code="2">\nCommand: npm test\nIt finished after 4s with exit code 2.\n</background-task>',
    metadata: { background_task: { ...runningShell, state: 'failed', exit_code: 2, event: 'finished', title: 'npm test' } },
  };
  const notice = backgroundTaskNotice(message.metadata);
  assert.equal(backgroundTaskNoticeCode(notice), 'background_command_failed');
  const presented = presentBackgroundTaskMessage(message);
  assert.equal(presented.role, 'system');
  assert.equal(presented.metadata.system_notice.code, 'background_command_failed');
  assert.equal(presented.metadata.system_notice.params.task, 'npm test');
  assert.equal(presented.content.startsWith('Command: npm test'), true);
  assert.equal(presented.content.includes('</background-task>'), false);
  // 普通用户消息原样返回。
  const plain = { role: 'user', content: 'hi', metadata: {} };
  assert.equal(presentBackgroundTaskMessage(plain), plain);
  // 子代理 / 新输出 / 静默提醒各有自己的标题。
  assert.equal(backgroundTaskNoticeCode({ kind: 'agent', state: 'completed' }), 'background_agent_finished');
  assert.equal(backgroundTaskNoticeCode({ kind: 'shell', event: 'output', state: 'running' }), 'background_command_output');
  assert.equal(backgroundTaskNoticeCode({ kind: 'shell', event: 'quiet', state: 'running' }), 'background_command_quiet');
  assert.equal(backgroundTaskBody('plain'), 'plain');
});

run('摘要面板:后台命令显示命令本身并用终端图标,停止请求标为 stopping', () => {
  const command = { ...normalizeCommandTask(runningShell), stopPending: true };
  const rows = summaryRunningTasks([command], 4000);
  assert.equal(rows[0].kind, 'command');
  assert.equal(rows[0].title, 'npm run dev');
  assert.equal(rows[0].stopping, true);
  assert.equal(rows[0].elapsed, '03s');
});

// 回归:后台命令结束通知唤醒的新回合把前一回合整体折进「已处理」,前一回合
// task_complete 的总结连展开都看不到(实测:「命令已转入后台」那句总结消失)。
// 通知本身不画进对话(用户反馈「后台命令已完成」那一行纯属干扰),结果在摘要面板
// 和详情栏的后台任务里看;它仍负责把唤醒的新回合与前一回合分开。
run('后台通知唤醒新回合:前一回合保留自己的总结,通知行不显示', () => {
  const tool = (id, extra = {}) => ({
    kind: 'tool', id, ts: id * 1000,
    tool: { isDone: true, success: true, tool: 'bash', summary: { verb: 'Ran', object: 'ping', metrics: [] }, ...extra },
  });
  const taskComplete = (id, summary) => ({
    kind: 'tool', id, ts: id * 1000,
    tool: { isTaskComplete: true, isDone: true, success: true, tool: 'task_complete', summary: { object: summary } },
  });
  const notice = (id) => presentBackgroundTaskMessage({
    kind: 'msg', id, ts: id * 1000, role: 'user', content: ['<background-task id="bg_1">', 'Command: ping', '</background-task>'].join(String.fromCharCode(10)),
    metadata: { background_task: { ...runningShell, state: 'completed', exit_code: 0, event: 'finished' } },
  });
  const items = [
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: 'run ping' },
    tool(2), taskComplete(3, '已转入后台'),
    notice(4),
    { kind: 'msg', id: 5, ts: 5000, role: 'assistant', content: '退出码 0' },
  ];
  const projected = projectCollapsedTranscriptItems(items);
  // 期望:第一回合的总结行还在;通知行不出现;第二回合的答复跟在第一回合总结之后。
  const summaries = projected.filter((item) => item.kind === 'completion_summary');
  assert.equal(summaries.length, 1);
  assert.match(summaries[0].title, /已转入后台/);
  assert.equal(projected.some((item) => item.id === 4), false);
  assert.ok(projected.length - 1 > projected.indexOf(summaries[0]));
  assert.equal(projected[projected.length - 1].content, '退出码 0');

  // 场景:回合进行中(前面是工具结果)插入的通知不切分回合,仍只有一段「已处理」。
  const midTurn = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: 'run ping' },
    tool(2), notice(3), tool(4),
    { kind: 'msg', id: 5, ts: 5000, role: 'assistant', content: 'done' },
  ]);
  assert.equal(midTurn.filter((item) => item.mode === 'processed').length, 1);
  assert.equal(midTurn.some((item) => item.id === 3), false);

  // 场景:空闲会话刚被通知唤醒、模型还没出内容 → 不显示通知,只留实时活动行。
  const woken = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: 'run ping' },
    tool(2), taskComplete(3, '已转入后台'),
    notice(4),
  ], { ensureLiveActivity: true, deferTrailingToolSummary: true, liveTurnId: 't2' });
  assert.equal(woken.some((item) => item.id === 4), false);
  const last = woken[woken.length - 1];
  assert.equal(last.kind, 'activity_summary');
  assert.equal(last.live, true);
});

// ── 对话里的后台任务状态:挂在发起它的工具行上,折叠条跟着带炫光 ──────────

const startedBash = (id, taskId, extra = {}) => ({
  kind: 'tool', id, ts: id * 1000,
  tool: {
    isDone: true, success: true, tool: 'bash',
    summary: { verb: 'Ran', object: 'npm run dev', metrics: [['background', 'true']] },
    metadata: { background_task_id: taskId, background_origin: 'requested' },
    ...extra,
  },
});
const plainTool = (id) => ({
  kind: 'tool', id, ts: id * 1000,
  tool: { isDone: true, success: true, tool: 'file_read', summary: { verb: 'Read', object: 'a.txt', metrics: [] } },
});
const finishTurn = (id) => ({
  kind: 'tool', id, ts: id * 1000,
  tool: { isTaskComplete: true, isDone: true, success: true, tool: 'task_complete', summary: { object: '已转入后台' } },
});

run('工具行认领后台任务:只有启动者算,停止工具不算', () => {
  // 触发场景:bash 以 run_in_background 启动(或超时 / 手动转入后台),结果同时带任务 id 和来源。
  assert.equal(startedBackgroundTaskId(startedBash(1, 'bg_1').tool), 'bg_1');
  // 回归防线:stop_background_task 的结果也带 background_task_id(指被它停掉的任务),但没有来源。
  // 若把它也算进去,任务被停后「已停止」的出错样式会落到停止工具那一行上。
  assert.equal(startedBackgroundTaskId({ tool: 'stop_background_task', metadata: { background_task_id: 'bg_1' } }), '');
  // 普通工具、没有 metadata、metadata 形态不对 → 都不认领。
  assert.equal(startedBackgroundTaskId(plainTool(2).tool), '');
  assert.equal(startedBackgroundTaskId({ metadata: ['background_task_id'] }), '');
  assert.equal(startedBackgroundTaskId(null), '');
});

run('任务状态判定:运行中 / 失败或被停止 / 其余', () => {
  const running = normalizeCommandTask(runningShell);
  assert.equal(isBackgroundTaskRunning(running), true);
  assert.equal(isBackgroundTaskFailed(running), false);
  // 场景:非 0 退出码、被用户或模型停止 → 工具行按出错显示。
  assert.equal(isBackgroundTaskFailed(normalizeCommandTask({ ...runningShell, state: 'failed', exit_code: 2 })), true);
  assert.equal(isBackgroundTaskFailed(normalizeCommandTask({ ...runningShell, state: 'stopped' })), true);
  // 场景:正常结束 → 普通已完成行。
  const completed = normalizeCommandTask({ ...runningShell, state: 'completed', exit_code: 0 });
  assert.equal(isBackgroundTaskRunning(completed), false);
  assert.equal(isBackgroundTaskFailed(completed), false);
  // 场景:子代理任务没有 state 字段,中止也不算命令失败;查不到任务(旧会话、daemon 重启过)两个都是 false。
  assert.equal(isBackgroundTaskFailed({ id: 'child', status: 'aborted' }), false);
  assert.equal(isBackgroundTaskRunning(null), false);
  assert.equal(isBackgroundTaskFailed(null), false);
});

run('任务索引:命令与子代理合在一起,只有运行中的进 runningIds', () => {
  const index = indexBackgroundTasks([
    normalizeCommandTask(runningShell),
    normalizeCommandTask({ ...runningShell, id: 'bg_2', state: 'completed', exit_code: 0 }),
    { id: 'child-1', status: 'running' },
    { id: 'child-2', status: 'completed' },
    { status: 'running' },
  ]);
  assert.deepEqual([...index.byId.keys()], ['bg_1', 'bg_2', 'child-1', 'child-2']);
  assert.deepEqual([...index.runningIds], ['bg_1', 'child-1']);
  // 没有任务时返回同一个空索引对象:绝大多数会话没有后台任务,不能每次渲染都换新对象。
  assert.equal(indexBackgroundTasks([]), EMPTY_BACKGROUND_TASK_INDEX);
  assert.equal(indexBackgroundTasks(null), EMPTY_BACKGROUND_TASK_INDEX);
});

// 回归:回合早已结束、后台命令还在跑时,对话末尾曾画一行带转圈的「1 个后台任务正在运行」,
// 用户以为回合没结束、不敢发下一条。现在工具行一直折在「已处理」里,由折叠条带炫光表达。
run('折叠条炫光:包着未结束后台任务的每一层都算,别的回合不算', () => {
  const projected = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: '起个 dev server' },
    plainTool(2), startedBash(3, 'bg_1'), plainTool(4),
    { kind: 'msg', id: 5, ts: 5000, role: 'assistant', content: '已在后台启动' },
    finishTurn(6),
    { kind: 'msg', id: 7, ts: 7000, role: 'user', content: '看下 a.txt' },
    plainTool(8), plainTool(9),
    { kind: 'msg', id: 10, ts: 10000, role: 'assistant', content: '看完了' },
    finishTurn(11),
  ]);
  const folds = projected.filter((item) => item.mode === 'processed');
  assert.equal(folds.length, 2);
  const running = new Set(['bg_1']);

  // 期望:第一回合的「已处理」包着 bg_1 → 炫光;第二回合没有后台任务 → 不炫光。
  assert.deepEqual(backgroundTaskIdsOfItem(folds[0]), ['bg_1']);
  assert.equal(itemHasRunningBackgroundTask(folds[0], running), true);
  assert.equal(itemHasRunningBackgroundTask(folds[1], running), false);

  // 期望:展开后,里面那层工具分组同样带炫光,最里面认领任务的只有那条 bash 行。
  const nestedFold = folds[0].detailItems.find((item) => item.kind === 'activity_summary');
  assert.ok(nestedFold, '三个连续工具在展开详情里应当合成一个工具分组');
  assert.equal(itemHasRunningBackgroundTask(nestedFold, running), true);
  assert.deepEqual(
    nestedFold.collapsedItems.filter((item) => itemHasRunningBackgroundTask(item, running)).map((item) => item.id),
    [3],
  );

  // 期望:任务结束(不在运行集合里)或根本没有运行中的任务 → 各层一起停。
  assert.equal(itemHasRunningBackgroundTask(folds[0], new Set(['bg_other'])), false);
  assert.equal(itemHasRunningBackgroundTask(folds[0], new Set()), false);
  assert.equal(itemHasRunningBackgroundTask(folds[0], null), false);
  assert.equal(itemHasRunningBackgroundTask(null, running), false);
});

run('折叠条炫光:后台子代理按子会话 id 认领', () => {
  // 触发场景:spawn_subagent wait=false,回合结束后子会话还在跑。子代理工具项在投影里
  // 合成「调用了 N 个智能体」分组,任务 id 就是子会话 id。
  const spawn = (id, childId) => ({
    kind: 'tool', id, ts: id * 1000,
    tool: {
      isDone: true, success: true, tool: 'spawn_subagent',
      summary: { verb: 'Spawned', object: '查资料', metrics: [] },
      metadata: { subagent_session_id: childId, background_task_id: childId, background_origin: 'requested' },
    },
  });
  const projected = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: '派两个子代理' },
    spawn(2, 'child-1'), spawn(3, 'child-2'),
    { kind: 'msg', id: 4, ts: 4000, role: 'assistant', content: '已派出' },
    finishTurn(5),
  ]);
  const fold = projected.find((item) => item.mode === 'processed');
  assert.deepEqual([...backgroundTaskIdsOfItem(fold)].sort(), ['child-1', 'child-2']);
  // 期望:只要还有一个子会话在跑就炫光;都结束后停。
  assert.equal(itemHasRunningBackgroundTask(fold, new Set(['child-2'])), true);
  assert.equal(itemHasRunningBackgroundTask(fold, new Set(['someone-else'])), false);
});
