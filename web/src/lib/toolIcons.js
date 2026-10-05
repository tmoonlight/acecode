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
