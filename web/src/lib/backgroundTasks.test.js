import assert from 'node:assert/strict';
import {
  applyBackgroundTaskEvent,
  backgroundableToolStartedAt,
  commandTaskStatusLabel,
  isCommandTask,
  markCommandTaskStopping,
  mergeCommandTaskList,
  normalizeCommandTask,
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
run('后台通知唤醒新回合:前一回合保留自己的总结,通知行开始新回合', () => {
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
  // 期望:第一回合的总结行还在;通知行独立显示;第二回合的答复在通知之后。
  const summaries = projected.filter((item) => item.kind === 'completion_summary');
  assert.equal(summaries.length, 1);
  assert.match(summaries[0].title, /已转入后台/);
  const noticeIndex = projected.findIndex((item) => item.id === 4);
  assert.ok(noticeIndex > projected.indexOf(summaries[0]));
  assert.equal(projected[projected.length - 1].content, '退出码 0');

  // 场景:回合进行中(前面是工具结果)插入的通知不切分回合,仍只有一段「已处理」。
  const midTurn = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: 'run ping' },
    tool(2), notice(3), tool(4),
    { kind: 'msg', id: 5, ts: 5000, role: 'assistant', content: 'done' },
  ]);
  assert.equal(midTurn.filter((item) => item.mode === 'processed').length, 1);
  assert.ok(midTurn.some((item) => item.id === 3));
});
