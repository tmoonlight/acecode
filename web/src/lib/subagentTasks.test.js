// subagentTasks.js 的单元测试。
//
// 后台任务面板的全部状态转移都在纯函数层:REST 快照归一化、refetch 合并
// (保留本地中止标记)、WS 事件增量、分组/徽标计数、
// 列表行显示格式化。DOM 端(SubagentPanel.jsx)只做结构→className 映射。
//
// 覆盖:
//  - normalizeSubagentTask:busy/status 判定、tokens 提取、坏输入容错
//  - mergeSubagentTaskList:refetch 保留 aborted 标记、排序
//  - applySubagentSessionEvent:busy_changed 状态迁移、usage 单调递增、
//    工具事件与无关事件返回原引用
//  - markSubagentTaskAborted 后 busy_changed(false) 不把状态改回 completed
//  - 分组 / runningCount / 格式化函数边界

import assert from 'node:assert/strict';
import {
  SUBAGENT_TASK_STATUS,
  applySubagentSessionEvent,
  formatElapsed,
  formatTaskTokens,
  markSubagentTaskAborted,
  mergeSubagentTaskList,
  normalizeSubagentTask,
  removeSubagentTask,
  runningSubagentCount,
  shouldRefreshSubagentTasksFromStatus,
  subagentAgentsOfItem,
  subagentAvatarStatus,
  subagentGroupAvatars,
  subagentTaskGroups,
  taskDisplayTitle,
  taskElapsedSeconds,
  taskRunningStatusLabel,
  taskTimeLabel,
  taskTokensLabel,
} from './subagentTasks.js';
import { projectCollapsedTranscriptItems } from './transcriptProjection.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

function makeSession(overrides = {}) {
  return {
    id: 'child-1',
    parent_session_id: 'parent-1',
    title: '读取 test 文件夹内容',
    summary: 'list the folder',
    busy: false,
    status: 'idle',
    created_at: '2026-07-05T09:00:00Z',
    updated_at: '2026-07-05T09:00:39Z',
    turn_count: 1,
    session_token_usage: { total_tokens: 34000 },
    ...overrides,
  };
}

run('normalizeSubagentTask 提取快照字段,busy=false → completed', () => {
  const task = normalizeSubagentTask(makeSession());
  assert.equal(task.id, 'child-1');
  assert.equal(task.parentId, 'parent-1');
  assert.equal(task.status, SUBAGENT_TASK_STATUS.COMPLETED);
  assert.equal(task.tokens, 34000);
  assert.equal(task.turnCount, 1);
});

run('normalizeSubagentTask busy=true 或 status=running → running', () => {
  assert.equal(normalizeSubagentTask(makeSession({ busy: true })).status,
               SUBAGENT_TASK_STATUS.RUNNING);
  assert.equal(normalizeSubagentTask(makeSession({ status: 'running' })).status,
               SUBAGENT_TASK_STATUS.RUNNING);
});

run('normalizeSubagentTask 坏输入返回 null,不抛异常', () => {
  assert.equal(normalizeSubagentTask(null), null);
  assert.equal(normalizeSubagentTask({}), null);
  assert.equal(normalizeSubagentTask('x'), null);
});

run('mergeSubagentTaskList refetch 保留 aborted 标记', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = markSubagentTaskAborted(tasks, 'child-1');
  // refetch:同一会话的新快照(busy=false)进来,不应把「已中止」洗回 completed。
  const merged = mergeSubagentTaskList(tasks, [makeSession({ busy: false })]);
  assert.equal(merged[0].status, SUBAGENT_TASK_STATUS.ABORTED);
});

// 场景:子代理在跑,每调一次工具都会有 tool_start / tool_end 事件送到父会话页面。
// 期望:这些事件不改任务列表(返回同一个数组),调用方据此跳过 setState。
// 回归防线:列表曾显示「N tools · 当前工具」,每个工具事件都换一份新列表,整个对话跟着重画;
// 列表改成单行后不再显示这些,不能再为它们换列表。
run('子会话的工具事件不改任务列表', () => {
  const tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  for (const type of ['tool_start', 'tool_end']) {
    assert.equal(applySubagentSessionEvent(tasks, {
      type, session_id: 'child-1', payload: { tool: 'bash' },
    }), tasks);
  }
});

run('mergeSubagentTaskList 按创建时间倒序(新任务在前)', () => {
  const merged = mergeSubagentTaskList([], [
    makeSession({ id: 'old', created_at: '2026-07-05T08:00:00Z' }),
    makeSession({ id: 'new', created_at: '2026-07-05T10:00:00Z' }),
  ]);
  assert.deepEqual(merged.map((t) => t.id), ['new', 'old']);
});

// 场景:模型用 wait=false 派子代理,子会话刚建好、还没写元数据就被列出来,
// 列表里 created_at 是空串;之后这个任务已认识,不会再刷新列表。
// 期望:运行中的任务用第一次看到它的时间计时;后续刷新若仍缺 created_at 沿用这个时间,
// 拿到真实 created_at 时换成真实值;已结束且缺时间的任务不编造开始时间。
// 回归:曾经 createdAtMs 停在 0,摘要面板的耗时一直显示 00s 不动。
run('子会话列表缺 created_at 时,运行中任务按首次看到的时间计时', () => {
  const seenAt = Date.parse('2026-11-09T11:09:29Z');
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true, created_at: '' })], seenAt);
  assert.equal(tasks[0].createdAtMs, seenAt);
  assert.equal(taskElapsedSeconds(tasks[0], seenAt + 21000), 21);

  tasks = mergeSubagentTaskList(tasks, [makeSession({ busy: true, created_at: '' })], seenAt + 5000);
  assert.equal(tasks[0].createdAtMs, seenAt, '再次刷新仍缺时间时不重置计时起点');

  tasks = mergeSubagentTaskList(tasks, [makeSession({ busy: true, created_at: '2026-11-09T11:09:28Z' })], seenAt + 9000);
  assert.equal(tasks[0].createdAtMs, Date.parse('2026-11-09T11:09:28Z'));

  const settled = mergeSubagentTaskList([], [makeSession({ busy: false, created_at: '' })], seenAt);
  assert.equal(settled[0].createdAtMs, 0);
});

run('applySubagentSessionEvent busy_changed 完成迁移 + usage 单调更新', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'usage', session_id: 'child-1', payload: { total_tokens: 40000 },
  });
  assert.equal(tasks[0].tokens, 40000);
  // 更小的 usage(乱序旧帧)不回退。
  const same = applySubagentSessionEvent(tasks, {
    type: 'usage', session_id: 'child-1', payload: { total_tokens: 100 },
  });
  assert.equal(same[0].tokens, 40000);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'busy_changed', session_id: 'child-1', payload: { busy: false },
  });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.COMPLETED);
});

run('session_status 只为明确匹配当前 parent 的未知 child 触发刷新', () => {
  const known = new Set(['child-known']);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    session_id: 'child-new',
    parent_session_id: 'parent-1',
    payload: {
      session_id: 'child-new',
      parent_session_id: 'parent-1',
      busy: true,
    },
  }), true);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    payload: {
      session_id: 'child-new',
      parent_session_id: 'parent-2',
      busy: true,
    },
  }), false);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    payload: { session_id: 'unrelated', busy: true },
  }), false);
  assert.equal(shouldRefreshSubagentTasksFromStatus('parent-1', known, {
    type: 'session_status',
    payload: {
      session_id: 'child-known',
      parent_session_id: 'parent-1',
      busy: true,
    },
  }), false);
});

// 场景:蜂群模式（网状）用 agent_spawn 派子 agent(add-mesh-swarm-mode),子会话扁平挂在
// 根会话下,真实层级在 agent_path 里。
// 期望:任务对象带上 agentPath 供卡片显示层级;星型子任务没有路径,字段为空串。
run('网状子 agent 任务带 agent 路径', () => {
  assert.equal(normalizeSubagentTask(makeSession({ agent_path: '/root/lead/sub' })).agentPath,
    '/root/lead/sub');
  assert.equal(normalizeSubagentTask(makeSession()).agentPath, '');
});

run('已知 child 的 session_status 可直接完成任务状态', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'session_status',
    payload: {
      session_id: 'child-1',
      parent_session_id: 'parent-1',
      busy: false,
    },
  });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.COMPLETED);
});

run('applySubagentSessionEvent 无关会话/无关类型返回原数组引用', () => {
  const tasks = mergeSubagentTaskList([], [makeSession()]);
  assert.equal(applySubagentSessionEvent(tasks, {
    type: 'busy_changed', session_id: 'other', payload: { busy: true },
  }), tasks);
  assert.equal(applySubagentSessionEvent(tasks, {
    type: 'token', session_id: 'child-1', payload: { text: 'x' },
  }), tasks);
});

run('中止后 busy_changed(false) 不把「已中止」洗成 completed', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = markSubagentTaskAborted(tasks, 'child-1');
  tasks = applySubagentSessionEvent(tasks, {
    type: 'busy_changed', session_id: 'child-1', payload: { busy: false },
  });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.ABORTED);
});

run('session_updated 更新卡片标题(auto-title 生成到达)', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ title: '' })]);
  tasks = applySubagentSessionEvent(tasks, {
    type: 'session_updated', session_id: 'child-1', payload: { title: '新标题' },
  });
  assert.equal(tasks[0].title, '新标题');
});

run('分组与徽标计数', () => {
  const tasks = mergeSubagentTaskList([], [
    makeSession({ id: 'a', busy: true }),
    makeSession({ id: 'b', busy: false }),
    makeSession({ id: 'c', status: 'running' }),
  ]);
  const groups = subagentTaskGroups(tasks);
  assert.equal(groups.running.length, 2);
  assert.equal(groups.settled.length, 1);
  assert.equal(runningSubagentCount(tasks), 2);
});

run('removeSubagentTask 清除单个任务;未命中返回原引用', () => {
  const tasks = mergeSubagentTaskList([], [makeSession()]);
  assert.equal(removeSubagentTask(tasks, 'nope'), tasks);
  assert.equal(removeSubagentTask(tasks, 'child-1').length, 0);
});

run('formatTaskTokens / formatElapsed 边界', () => {
  assert.equal(formatTaskTokens(0), '0');
  assert.equal(formatTaskTokens(999), '999');
  assert.equal(formatTaskTokens(34000), '34.0k');
  assert.equal(formatTaskTokens(240000), '240k');
  assert.equal(formatElapsed(9), '09s');
  assert.equal(formatElapsed(83), '1m23s');
  assert.equal(formatElapsed(3700), '1h01m');
  const task = normalizeSubagentTask(makeSession());
  // 已结束任务耗时 = updated - created = 39s。
  assert.equal(taskElapsedSeconds(task), 39);
});

// 场景:后台任务页签里的子代理是单行列表:标题、token 数,时间靠右,运行中多一行状态。
// 期望:token 数没有用量时不显示;运行中的时间是已经跑了多久(每秒走),结束后是多久之前
// 结束的;状态那一行只有运行中才有,已结束(完成 / 中止)的行不显示状态。
run('子代理列表行:token 数、靠右的时间、运行中才有的状态行', () => {
  const now = Date.UTC(2026, 6, 5, 9, 10, 0);
  const running = { status: SUBAGENT_TASK_STATUS.RUNNING, createdAtMs: now - 24_000, tokens: 23200 };
  assert.equal(taskTokensLabel(running), '23.2k tokens');
  assert.equal(taskTokensLabel({ tokens: 0 }), '');
  assert.equal(taskTokensLabel(null), '');

  assert.equal(taskTimeLabel(running, now), '24s');
  assert.equal(taskTimeLabel(running, now + 1_000), '25s');
  const finished = {
    status: SUBAGENT_TASK_STATUS.COMPLETED, createdAtMs: now - 400_000, updatedAtMs: now - 6 * 60_000,
  };
  assert.equal(taskTimeLabel(finished, now), '6分钟前');
  assert.equal(taskTimeLabel({ ...finished, updatedAtMs: now - 3_000 }, now), '刚刚');
  // 没有结束时间的旧记录退到开始时间;两个都没有就不显示。
  assert.equal(taskTimeLabel({ status: SUBAGENT_TASK_STATUS.COMPLETED, createdAtMs: now - 2 * 3_600_000 }, now), '2小时前');
  assert.equal(taskTimeLabel({ status: SUBAGENT_TASK_STATUS.COMPLETED }, now), '');

  assert.equal(taskRunningStatusLabel(running), '运行中');
  assert.equal(taskRunningStatusLabel({ ...running, abortPending: true }), '正在停止…');
  assert.equal(taskRunningStatusLabel(finished), '');
  assert.equal(taskRunningStatusLabel({ status: SUBAGENT_TASK_STATUS.ABORTED }), '');
});

// 场景:子代理头像右上角画状态图标(运行中三个点 / 完成一颗星 / 中止红色感叹号)。
// 期望:运行中(点了停止、还没停下来也算)→ running;已完成 → done;已中止 → stopped;
// 任务列表里查不到这个子代理(旧会话、子会话已删)→ 不画,不靠别的标记猜。
run('头像状态图标:按任务状态决定,查不到任务不画', () => {
  assert.equal(subagentAvatarStatus({ status: SUBAGENT_TASK_STATUS.RUNNING }), 'running');
  assert.equal(subagentAvatarStatus({ status: SUBAGENT_TASK_STATUS.RUNNING, abortPending: true }), 'running');
  assert.equal(subagentAvatarStatus({ status: SUBAGENT_TASK_STATUS.COMPLETED }), 'done');
  assert.equal(subagentAvatarStatus({ status: SUBAGENT_TASK_STATUS.ABORTED }), 'stopped');
  assert.equal(subagentAvatarStatus(null), '');
  assert.equal(subagentAvatarStatus(undefined), '');
  assert.equal(subagentAvatarStatus({}), '');
});

// 场景:对话里「调用了 N 个智能体」一行前面画这几个子代理的头像。
// 期望:拿到子会话 id 的子代理每个一张、顺序与调用顺序一致,绑定了专家的带上专家信息,
// 各自带上自己的状态(任务列表里查不到的没有状态);还没有子会话 id 的不画;
// 个数多时只画前几张(总数已经写在文案里)。
run('子代理分组行的头像:按调用顺序、带专家信息和状态、有上限', () => {
  const expert = { avatar_identity: 'expert:reviewer' };
  const tasksById = new Map([
    ['child-b', { id: 'child-b', expert, status: SUBAGENT_TASK_STATUS.RUNNING }],
    ['child-c', { id: 'child-c', status: SUBAGENT_TASK_STATUS.COMPLETED }],
  ]);
  assert.deepEqual(subagentGroupAvatars([
    { sessionId: 'child-a' },
    { sessionId: '' },
    { sessionId: 'child-b' },
    { sessionId: 'child-c' },
  ], tasksById), [
    { sessionId: 'child-a', expert: null, status: '' },
    { sessionId: 'child-b', expert, status: 'running' },
    { sessionId: 'child-c', expert: null, status: 'done' },
  ]);
  const many = Array.from({ length: 9 }, (_, index) => ({ sessionId: `child-${index}` }));
  assert.equal(subagentGroupAvatars(many, new Map()).length, 6);
  assert.deepEqual(subagentGroupAvatars(many, null, 2).map((avatar) => avatar.sessionId), ['child-0', 'child-1']);
  assert.deepEqual(subagentGroupAvatars(null, null), []);
});

// 场景:一个回合里模型读了文件、又派了两个子代理,回合结束后折成「已处理」。展开后先看到一行
// 工具汇总(「读取 1 个文件，调用了 2 个智能体」),再展开才是子代理分组本身。
// 期望:工具汇总行和子代理分组都能取到这两个子代理(行首画头像用),顺序与调用顺序一致;
// 不含子代理的汇总行、普通工具行取到的是空。
// 回归:头像曾经只画在最里面的分组行上,外面那层同样写着「调用了 N 个智能体」却是通用图标。
run('工具汇总行里包着的子代理:汇总行与分组行取到同一批,没有子代理时为空', () => {
  const tool = (id, name, extra = {}) => ({
    kind: 'tool', id, ts: id * 1000,
    tool: { isDone: true, success: true, tool: name, summary: { verb: 'Did', object: 'x', metrics: [] }, ...extra },
  });
  const spawn = (id, childId) => tool(id, 'spawn_subagent', {
    metadata: { subagent_session_id: childId, background_task_id: childId, background_origin: 'requested' },
  });
  const finish = (id) => tool(id, 'task_complete', { isTaskComplete: true });
  const projected = projectCollapsedTranscriptItems([
    { kind: 'msg', id: 1, ts: 1000, role: 'user', content: '先看文件再派两个子代理' },
    tool(2, 'file_read'), spawn(3, 'child-1'), spawn(4, 'child-2'),
    { kind: 'msg', id: 5, ts: 5000, role: 'assistant', content: '已派出' },
    finish(6),
    { kind: 'msg', id: 7, ts: 7000, role: 'user', content: '再看两个文件' },
    tool(8, 'file_read'), tool(9, 'file_read'),
    { kind: 'msg', id: 10, ts: 10000, role: 'assistant', content: '看完了' },
    finish(11),
  ]);
  const folds = projected.filter((item) => item.mode === 'processed');
  assert.equal(folds.length, 2);
  const sessionIds = (item) => subagentAgentsOfItem(item).map((agent) => agent.sessionId);

  const toolFold = folds[0].detailItems.find((item) => item.kind === 'activity_summary');
  assert.ok(toolFold, '文件读取和子代理调用应当合成一行工具汇总');
  assert.deepEqual(sessionIds(toolFold), ['child-1', 'child-2']);
  const group = toolFold.collapsedItems.find((item) => item.kind === 'subagent_group');
  assert.ok(group);
  assert.deepEqual(sessionIds(group), ['child-1', 'child-2']);
  // 同一个行对象重复取是同一个数组(每次渲染都会取,不能每次都新建)。
  assert.equal(subagentAgentsOfItem(toolFold), subagentAgentsOfItem(toolFold));

  const plainFold = folds[1].detailItems.find((item) => item.kind === 'activity_summary');
  assert.ok(plainFold);
  assert.deepEqual(sessionIds(plainFold), []);
  assert.deepEqual(sessionIds(tool(20, 'file_read')), []);
  assert.deepEqual(sessionIds(null), []);
});

run('taskDisplayTitle 标题 → 摘要 → id 逐级退化', () => {
  assert.equal(taskDisplayTitle({ title: 'T', summary: 'S', id: 'i' }), 'T');
  assert.equal(taskDisplayTitle({ title: '', summary: 'S', id: 'i' }), 'S');
  assert.equal(taskDisplayTitle({ title: '', summary: '', id: 'i' }), 'i');
  // auto-title 生成失败的 "[Error] ..." 占位对用户无意义,退到摘要。
  assert.equal(taskDisplayTitle({ title: '[Error] Failed to parse response JSON', summary: 'S', id: 'i' }), 'S');
});

run('停止请求保留运行订阅直到确认，迟到 busy 不会重置请求', () => {
  let tasks = mergeSubagentTaskList([], [makeSession({ busy: true })]);
  tasks = markSubagentTaskAborted(tasks, 'child-1');
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.RUNNING);
  assert.equal(tasks[0].abortPending, true);
  tasks = applySubagentSessionEvent(tasks, { type: 'busy_changed', session_id: 'child-1', payload: { busy: true } });
  assert.equal(tasks[0].abortPending, true);
  tasks = applySubagentSessionEvent(tasks, { type: 'done', session_id: 'child-1', payload: { outcome: 'aborted' } });
  assert.equal(tasks[0].status, SUBAGENT_TASK_STATUS.ABORTED);
  assert.equal(tasks[0].abortPending, false);
});

console.log('subagentTasks.test.js: all tests passed');
