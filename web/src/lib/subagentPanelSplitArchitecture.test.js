import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8').replace(/\r\n?/g, '\n');
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

// 场景:后台任务(后台命令 + 子代理)的列表和子会话内容。
// 期望:不再是对话区旁边可拖宽的独立分栏,而是详情栏里的「后台任务」页签;ChatView 只在
// 该页签激活时渲染 BackgroundTasksView 并交给 PreviewDetailsPanel,正在看的任务记在页签状态里。
// 回归:曾经是 ChatView 里单独一列(App 持久化宽度、ChatView 画拖动分隔条),用户要求并进详情栏。
run('background tasks live in a detail panel tab instead of a split pane', () => {
  const app = source('App.jsx');
  const chat = source('components/ChatView.jsx');
  const details = source('components/PreviewDetailsPanel.jsx');
  const view = source('components/SubagentPanel.jsx');

  assert.doesNotMatch(app, /onSubagentPanelResize|setSubagentPanelWidth|subagentPanelWidth=/);
  assert.doesNotMatch(chat, /data-subagent-splitter|startSubagentPanelResize|subagentPanelOpen|<SubagentPanel/);
  assert.doesNotMatch(chat, /subagentOpen:/);
  assert.match(chat, /activePreview\?\.type === PREVIEW_TAB_TYPES\.BACKGROUND_TASKS \? \(\s*<BackgroundTasksView/);
  assert.match(chat, /backgroundTasksContent=\{backgroundTasksContent\}/);
  assert.match(chat, /openBackgroundTasksTab\(prev, \{/);
  assert.match(chat, /updateBackgroundTasksTab\(prev, \{ sessionId: sid, taskId \}\)/);
  assert.match(details, /if \(active\.type === PREVIEW_TAB_TYPES\.BACKGROUND_TASKS\) \{\s*return backgroundTasksContent;/);
  assert.match(details, /PREVIEW_TAB_TYPES\.BACKGROUND_TASKS\) return '后台任务'/);
  assert.match(view, /export function BackgroundTasksView\(\{ tasks = \[\], taskId = '', onSelectTask,/);
  assert.doesNotMatch(view, /export function SubagentPanel|style=\{\{ width \}\}/);
});

// 场景:用户从三处打开后台任务:会话三点菜单、对话里的智能体分组卡片、摘要面板(运行中任务行 /「N 已完成」)。
// 期望:都走 openBackgroundTasksPreview:三点菜单保留页签里正在看的任务,分组卡片与任务行定位到该任务,
// 「N 已完成」回到任务列表。
run('every background task entry opens the detail panel tab', () => {
  const chat = source('components/ChatView.jsx');
  const summary = source('components/SummaryPanel.jsx');
  assert.match(chat, /id: 'background_tasks', label: '后台任务'[\s\S]*?onSelect: \(\) => openBackgroundTasksPreview\(\)/);
  assert.match(chat, /onOpenSubagent=\{openBackgroundTasksPreview\}/);
  assert.match(chat, /const openSummaryTask = useStableCallback\(\(taskId\) => \{\s*closeSummaryPopup\(\);\s*openBackgroundTasksPreview\(taskId\);/);
  assert.match(chat, /const openSummaryFinishedTasks = useStableCallback\(\(\) => \{\s*closeSummaryPopup\(\);\s*openBackgroundTasksPreview\(''\);/);
  assert.match(chat, /onOpenFinishedTasks=\{openSummaryFinishedTasks\}/);
  // 有运行中或已结束任务才显示分组,有已结束任务才显示「N 已完成」入口。
  assert.match(summary, /\(running\.length > 0 \|\| finishedCount > 0\) && \(/);
  assert.match(summary, /finishedCount > 0 && \(\s*<Row icon="check"/);
  assert.match(summary, /<Row icon="check" onClick=\{onOpenFinishedTasks\}>\s*\{finishedCount\} 已完成/);
});

// 场景:模型派子代理(spawn_subagent / 网状 agent_spawn),或子代理在后台跑着。
// 期望:后台任务面板不自动弹出,只在用户点三点菜单「后台任务」或点智能体分组卡片
// (含摘要面板里的子代理行)时打开;打开面板的调用点只有这两处。
// 回归:曾经父会话一收到 spawn_subagent 的 tool_start 就自动打开面板,模型更常派
// 子代理之后面板反复弹出、挤占对话区,用户要求去掉。
run('spawning a subagent does not open the background task panel by itself', () => {
  const taskHook = source('lib/useSubagentTasks.js');
  const chat = source('components/ChatView.jsx');

  assert.match(taskHook, /export function useSubagentTasks\(parentSessionId\) \{/);
  assert.doesNotMatch(taskHook, /tool_start|onSpawnStart/);
  assert.match(chat, /useSubagentTasks\(sid\);/);
  assert.doesNotMatch(chat, /onSpawnStart|openSubagentPanelForSpawn/);
  // 打开后台任务页签只有一个入口函数,调用点全是用户操作(见上一条测试)。
  const opens = chat.match(/openBackgroundTasksTab\(/g) || [];
  assert.equal(opens.length, 1);
  const callers = chat.match(/openBackgroundTasksPreview\(/g) || [];
  assert.equal(callers.length, 3);
});

// 场景:Desktop 的 daemon 同时服务多个工作区,子会话不在内存里(Desktop 重启后、
// 网状 agent 被换出)时打开后台任务里的子会话。
// 期望:子会话历史请求带上父会话所在工作区,daemon 才能去那个工作区读盘。
// 回归:曾经只带 sessionId,多工作区 daemon 只查自己的 cwd,面板显示
// 「加载失败:HTTP 404 SESSION_NOT_FOUND」,子会话内容看不到。
run('subagent transcript reads the parent workspace so unloaded children still open', () => {
  const panel = source('components/SubagentPanel.jsx');
  const chat = source('components/ChatView.jsx');
  const hook = source('lib/useSubagentTasks.js');

  assert.match(panel, /function SubagentTranscriptView\(\{ task, workspaceHash = '', messageAutoCollapse \}\)/);
  assert.match(panel, /sessionId: task\.id,\s*workspaceHash,/);
  assert.match(panel, /workspaceHash=\{workspaceHash\}/);
  assert.match(chat, /workspaceHash=\{subagentWorkspaceHash\}/);
  assert.match(hook, /useSubagentTasks\(parentSessionId\)/);
});

// 场景:后台任务面板里已结束的子任务(星型后台任务、网状 agent)。
// 期望:面板没有任何单独归档 / 清除 / 删除入口,子任务始终跟随主会话留在面板里,
// 只在主会话归档后被永久删除时由 daemon 一起删除(用户决策:事后分析要完整上下文)。
// 回归:曾经有「清除」(直接永久删除),后来改成「归档」(从面板收起),
// 两种都会让子会话离开主会话,用户要求整个入口去掉。
run('settled subagent tasks stay in the panel with no archive or delete entry', () => {
  const panel = source('components/SubagentPanel.jsx');
  const hook = source('lib/useSubagentTasks.js');
  const chat = source('components/ChatView.jsx');

  assert.doesNotMatch(hook, /clearSettled|archiveSession|archiveWorkspaceSession|purgeSession|purgeTask/);
  // 旧按钮文案是 JSX 字符串 '清除' / '归档'(注释里出现这两个词不算)。
  assert.doesNotMatch(panel, /onClearSettled|clearSettled|['>]归档|['>]清除/);
  assert.doesNotMatch(chat, /onClearSettled|clearSettled/);
});

// 场景:后台任务页签的列表里有子代理,也有后台命令。
// 期望:子代理是单行无边框的行(头像、标题、token 数、靠右的时间,运行中多一行状态),
// 点整行看会话,不再有「查看会话」链接和工具统计;后台命令保持原来的卡片与「查看输出」。
// 回归:子代理曾是带边框的多行卡片(Agent / Completed / tokens · tools / 查看会话),
// 任务一多列表很长,用户要求改成单行列表,后台命令明确要求不动。
run('sub-agent tasks render as borderless single-line rows while commands keep their cards', () => {
  const panel = source('components/SubagentPanel.jsx');
  const row = panel.slice(panel.indexOf('function TaskRow('), panel.indexOf('function CommandTaskCard('));
  assert.ok(row.length > 0);
  assert.match(row, /<SubagentAvatar sessionId=\{task\.id\} expert=\{task\.expert\} status=\{task\.status === SUBAGENT_TASK_STATUS\.COMPLETED \? '' : subagentAvatarStatus\(task\)\}/);
  assert.match(row, /taskTokensLabel\(task\)/);
  assert.match(row, /taskTimeLabel\(task, nowMs\)/);
  assert.match(row, /\{status && <span[^>]*data-task-status="true"/);
  assert.match(row, /onClick=\{\(\) => onOpenTranscript\?\.\(task\)\}/);
  assert.doesNotMatch(row, /border-border|ace-shadow|查看会话|lastTool|toolCount/);
  // 网状蜂群的 agent 路径只进悬停提示,不占一行。
  assert.match(row, /title=\{task\.agentPath \|\| undefined\}/);
  assert.doesNotMatch(row, />\s*\{task\.agentPath\}/);

  const card = panel.slice(panel.indexOf('function CommandTaskCard('), panel.indexOf('function CommandOutputView('));
  assert.match(card, /rounded-lg border border-border[^"]*ace-shadow" data-background-command=\{task\.id\}/);
  assert.match(card, /查看输出/);
  assert.match(panel, /isCommandTask\(task\) \? \(\s*<CommandTaskCard[\s\S]*?\) : \(\s*<TaskRow/);
});

// 场景:对话里模型派了几个子代理;摘要面板里也列着运行中的子代理。
// 期望:「调用了 N 个智能体」一行前面是这几个子代理的头像,展开后的每一行、摘要面板的
// 子代理行也带头像,三处用同一个组件(同一个子代理是同一张脸)。
// 回归:头像曾经只给绑定了专家的子会话,普通子代理前面是通用图标,分不出谁是谁。
run('sub-agent portraits lead the transcript group line, its rows and the summary rows', () => {
  const group = source('components/SubagentGroupBlock.jsx');
  const line = source('components/ActivityLine.jsx');
  const summary = source('components/SummaryPanel.jsx');
  const avatar = source('components/PixelAvatar.jsx');

  assert.match(group, /const avatars = subagentGroupAvatars\(agents, byId\);/);
  assert.match(group, /leading=\{avatars\.length > 0 \? <SubagentAvatars avatars=\{avatars\} \/> : null\}/);
  assert.match(group, /<SubagentAvatar sessionId=\{agent\.sessionId\} expert=\{task\?\.expert\} status=\{subagentAvatarStatus\(task\)\} \/>/);
  assert.match(line, /\{leading \? \(/);
  assert.match(summary, /iconNode=\{task\.kind === 'agent' && \(\s*<SubagentAvatar sessionId=\{task\.id\} expert=\{task\.expert\}/);
  assert.match(avatar, /const identity = subagentAvatarIdentity\(sessionId, expert\);/);

  // 外面包着的工具汇总行(同样写着「调用了 N 个智能体」)行首也是头像;「已处理」和进行中的行不画。
  const renderer = source('components/TranscriptItems.jsx');
  assert.match(renderer, /const agentAvatars = !live && item\?\.mode !== 'processed'\s*\? subagentGroupAvatars\(subagentAgentsOfItem\(item\), subagentTasksById\)/);
  assert.match(renderer, /leading=\{agentAvatars\.length > 0 \? <SubagentAvatars avatars=\{agentAvatars\} \/> : null\}/);
  assert.match(renderer, /<ActivitySummaryBlock[\s\S]*?subagentTasksById=\{subagentTasksById\}/);
});

// 场景:子代理头像出现在对话(分组行 + 展开后的每一行)、摘要面板、后台任务页签。
// 期望:四处都是 16px(组件默认值,调用处不再各写各的尺寸),右上角按任务状态画状态图标;
// 图标用办公室那份像素模板(miniSprite)画在单独的画布上,只有运行中的头像订阅共用的钟;
// 对话里智能体行原来的小圆点去掉(状态已经在头像上)。
// 回归:头像曾经在列表里是 20px、在分组行是 16px,用户要求缩到八成并带上办公室那样的状态图标。
run('sub-agent portraits share one size and carry the office status mark', () => {
  const avatar = source('components/PixelAvatar.jsx');
  const group = source('components/SubagentGroupBlock.jsx');
  const panel = source('components/SubagentPanel.jsx');
  const summary = source('components/SummaryPanel.jsx');
  const office = source('lib/officeCharacters.js');

  assert.match(avatar, /export function SubagentAvatar\(\{ sessionId = '', expert = null, status = '', size = 16, className = '' \}\)/);
  assert.match(avatar, /export function SubagentAvatars\(\{ avatars, size = 16 \}\)/);
  assert.match(avatar, /status=\{avatar\.status\}/);
  for (const [name, text] of [['group', group], ['panel', panel], ['summary', summary]]) {
    assert.doesNotMatch(text, /<SubagentAvatar[^>]*size=/, name);
  }
  assert.match(summary, /<SubagentAvatar sessionId=\{task\.id\} expert=\{task\.expert\} status="running" \/>/);

  assert.match(office, /miniSprite \} = globalThis\.ACEOfficeCharacters/);
  assert.match(avatar, /context\.drawImage\(miniSprite\(sprite\.name, sprite\.color\)\.cv, sprite\.x, sprite\.y\)/);
  assert.match(avatar, /return status === 'running' \? subscribeAvatarStatusClock\(paint\) : undefined;/);
  assert.match(avatar, /avatarNodOffsetPx\(size, window\.devicePixelRatio\)/);
  // 状态换掉或卸载时先把点头的位移清掉,不会停在低头的那一拍。
  assert.match(avatar, /if \(body\) body\.style\.transform = '';/);

  const row = group.slice(group.indexOf('function AgentRow('), group.indexOf('export function SubagentGroupBlock('));
  assert.doesNotMatch(row, /rounded-full|agentRunning/);
});

console.log('subagentPanelSplitArchitecture.test.js: all tests passed');
