import { interfaceIconSvg } from './interfaceIcons.js';

const GROUPS = [
  ['OpenFile', ['file_read', 'read_file', 'read', 'list_dir', 'ls', 'read_resource']],
  ['Save', ['file_write', 'write_file', 'write']],
  ['Edit', ['file_edit', 'edit_file', 'edit', 'apply_patch', 'multiedit']],
  ['Search', ['grep', 'file_search', 'glob', 'search_files', 'find_files']],
  ['BrowserGlobe', ['web_search', 'web_fetch', 'fetch', 'browse', 'open_url']],
  ['TerminalReadWrite', ['bash', 'shell', 'exec_command', 'execute_command', 'write_stdin']],
  ['Embedding', ['spawn_subagent', 'wait_subagent', 'agent_spawn', 'agent_wait', 'agent_list', 'agent_send_message', 'agent_followup_task', 'agent_interrupt']],
  ['StatusHelp', ['askuserquestion', 'ask_user_question']],
  ['List', ['todowrite', 'todo_write', 'todo_read']],
  ['StatusOK', ['task_complete']],
  ['Eye', ['vision_analyze', 'show_image', 'view_image']],
  ['Extension', ['skill', 'skill_view', 'skills_list']],
];
const BY_TOOL = new Map(GROUPS.flatMap(([icon, names]) => names.map(name => [name, icon])));
const LEGACY = new Map([['R','OpenFile'],['W','Save'],['E','Edit'],['S','Search'],['$','TerminalReadWrite'],['*','Tool'],['D','StatusOK'],['!','StatusWarning']]);

// One mapping and SVG source for chat, background tasks and the native office.
export function toolIconName(tool = '', summaryIcon = '') {
  const name = String(tool).toLowerCase().replace(/^functions\./, '');
  if (name.startsWith('mcp__') || name.startsWith('mcp:')) return 'MCP';
  return BY_TOOL.get(name) || LEGACY.get(summaryIcon) || 'Tool';
}

export function toolIconSvg(tool = '', summaryIcon = '') {
  return interfaceIconSvg(toolIconName(tool, summaryIcon), 14);
}

// Short present-tense verbs for the desktop office speech bubbles.
const VERB_BY_ICON = new Map([
  ['OpenFile', '读取'], ['Save', '写入文件'], ['Edit', '修改代码'], ['Search', '搜索代码'],
  ['BrowserGlobe', '查阅网页'], ['TerminalReadWrite', '运行命令'], ['Embedding', '协作'],
  ['StatusHelp', '提问'], ['List', '更新待办'], ['StatusOK', '收尾'], ['Eye', '看图'],
  ['Extension', '使用技能'], ['MCP', '调用 MCP'], ['Tool', '调用工具'],
]);
const VERB_BY_TOOL = new Map([
  ['spawn_subagent', '派发任务'], ['agent_spawn', '派发任务'], ['wait_subagent', '等成员回报'],
  ['agent_wait', '等成员回报'], ['agent_send_message', '发消息'], ['agent_followup_task', '追加任务'],
  ['agent_interrupt', '叫停成员'], ['agent_list', '查看成员'],
]);
export function toolActivityVerb(tool = '') {
  const name = String(tool).toLowerCase().replace(/^functions\./, '');
  return VERB_BY_TOOL.get(name) || VERB_BY_ICON.get(toolIconName(tool)) || '调用工具';
}
