import { tr } from '../i18n/index.js';
import { builtinTranscriptToolNames, toolTranscriptEnUS } from '../i18n/catalogs/toolTranscript.js';
import { basenameForPath } from './selectionChatContext.js';
import { compactOneLinePreview } from './compactMessagePreview.js';
import { formatBytes, formatElapsed } from './format.js';
import { shellCommandPresentation } from './shellCommandPresentation.js';

const BUILTINS = new Set(builtinTranscriptToolNames);
const hasOwn = (object, key) => Object.prototype.hasOwnProperty.call(object, key);
const ALIASES = {
  read: 'file_read', read_file: 'file_read', write: 'file_write', write_file: 'file_write',
  edit: 'file_edit', edit_file: 'file_edit', multiedit: 'file_edit',
  ask_user_question: 'askuserquestion', todo_write: 'todowrite', skill: 'skill_view',
  view_image: 'show_image', shell: 'bash', exec_command: 'bash', execute_command: 'bash',
};
const FILE_TOOLS = new Set(['file_read', 'file_write', 'file_edit', 'show_image', 'vision_analyze', 'lsp']);
const PATH_KEYS = new Set(['file_path', 'path', 'image_path', 'app_path', 'project', 'workspace', 'cwd', 'output_path']);
const ARG_LABELS = {
  file_path: 'path', image_path: 'path', app_path: 'path', output_path: 'path',
  include_pattern: 'files', head_limit: 'limit', max_results: 'limit',
  thread_id: 'target', session_id: 'target', bundle_id: 'target', task_name: 'name',
  mouse_button: 'button', model_name: 'model', secondary_action: 'action',
  threadId: 'target', timeoutMs: 'timeout_ms', turnLimit: 'limit',
  includeArchived: 'includeArchived', includeOutputs: 'includeOutputs', noWorkspace: 'noWorkspace',
};
const METRIC_KEYS = { time: 'elapsed', bytes: 'size', encoding: 'enc', line_ending: 'eol', '+': '+', '-': '-' };
const ENUM_FIELDS = new Set(['cache', 'partial', 'truncated', 'mode', 'hint', 'sandbox', 'status', 'quality', 'enabled', 'pinned', 'archived', 'aborted', 'timeout', 'includeArchived', 'includeOutputs', 'noWorkspace']);
const SENSITIVE_KEY = /(?:password|passwd|secret|token|authorization|credential|private.?key|api.?key)/i;

export function canonicalTranscriptTool(entry = {}) {
  if (!entry) return '';
  const raw = String(entry.tool || entry.metadata?.tool_name || '').replace(/^functions\./i, '').toLowerCase();
  if (raw) return ALIASES[raw] || raw;
  if (entry.askUserQuestionResult) return 'askuserquestion';
  const verb = String(entry.summary?.verb || '').toLowerCase();
  if (BUILTINS.has(verb) || ALIASES[verb]) return ALIASES[verb] || verb;
  if (verb === 'ran') return 'bash';
  if (verb === 'wrote') return 'file_write';
  if (verb === 'edited' && entry.hunks?.length) return 'file_edit';
  if (verb === 'created' && entry.hunks?.length) return 'file_write';
  if (verb === 'complete' || verb === 'done') return 'task_complete';
  return '';
}

export function toolArguments(value) {
  if (typeof value === 'string') {
    try { return toolArguments(JSON.parse(value)); } catch { return {}; }
  }
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
}

function textValue(value) {
  if (typeof value === 'string') return value.trim();
  if (typeof value === 'number' || typeof value === 'boolean') return String(value);
  if (Array.isArray(value)) return value.filter(v => typeof v === 'string').join(', ');
  return '';
}

function select(args, keys) {
  for (const key of keys) {
    const value = textValue(args[key]);
    if (value) return { key, value };
  }
  return { key: '', value: '' };
}

function primaryObject(tool, args, entry, t) {
  let keys;
  if (FILE_TOOLS.has(tool)) keys = ['file_path', 'path', 'image_path', 'image_paths'];
  else if (tool === 'grep' || tool === 'glob') keys = ['pattern'];
  else if (tool === 'bash') return { key: 'command', value: shellCommandPresentation({ ...entry, tool })?.command || '' };
  else if (tool === 'web_search') keys = ['query'];
  else if (tool === 'image_generate') keys = ['prompt'];
  else if (tool === 'apply_patch') {
    const paths = [...new Set((Array.isArray(entry.hunks) ? entry.hunks : []).map(h => h?.file).filter(Boolean))];
    if (!paths.length) {
      const patch = String(args.patch || args.input || '');
      for (const match of patch.matchAll(/^\*\*\* (?:Add|Update|Delete) File: (.+)$/gm)) paths.push(match[1].trim());
    }
    return { key: '', value: paths.length === 1 ? paths[0] : paths.length ? t('toolTranscript.fileCount', { count: paths.length }) : '', path: paths.length === 1 };
  } else if (tool.startsWith('browser_')) keys = ['url', 'key', 'name', 'target', 'selector', 'page_id'];
  else if (tool.startsWith('computer_')) keys = ['app', 'key', 'window'];
  else if (tool === 'ios_simulator') keys = ['device', 'bundle_id', 'app_path', 'url', 'key', 'name'];
  else if (tool === 'ios_simulator_build') keys = ['scheme', 'project', 'workspace'];
  else if (tool === 'skill_view' || tool.startsWith('memory_')) keys = ['name', 'query'];
  else if (tool === 'create_workspace') keys = ['path', 'cwd', 'name'];
  else if (tool === 'create_goal') keys = ['objective'];
  else if (tool === 'update_goal') keys = ['status'];
  else if (tool === 'create_thread' || tool === 'set_thread_title') keys = ['title'];
  else if (tool === 'spawn_subagent' || tool === 'agent_spawn') keys = ['task_name', 'description'];
  else if (tool === 'theme_create' || tool === 'create_scheduled_task' || tool === 'enterworktree') keys = ['name', 'title'];
  else keys = ['title', 'name', 'target', 'threadId', 'thread_id', 'session_id'];
  const selected = select(args, keys);
  if (selected.value) return selected;
  // Native summaries can supply old-history objects. Generic fallbacks are an
  // unlabelled concatenation of arguments and must not leak back into the row.
  const verb = String(entry.summary?.verb || '').toLowerCase();
  const object = textValue(entry.summary?.object);
  if (object && Object.keys(args).length === 0 && (!BUILTINS.has(verb) || FILE_TOOLS.has(tool))) {
    return { key: '', value: BUILTINS.has(verb) ? object.split(' · ')[0] : object };
  }
  return { key: '', value: '' };
}

function metricPairs(metrics) {
  return (Array.isArray(metrics) ? metrics : []).map(m => Array.isArray(m)
    ? [m[0], m[1]] : [m?.label || m?.key || m?.name, m?.value]);
}

export function presentTool(entry = {}, t = tr, { elapsed = entry?.elapsed || 0 } = {}) {
  entry = entry || {};
  const tool = canonicalTranscriptTool(entry);
  const builtin = BUILTINS.has(tool);
  const args = toolArguments(entry.args);
  let label = builtin ? t(`toolTranscript.names.${tool}`)
    : String(entry.tool || entry.summary?.verb || t('toolTranscript.unknown'));
  const action = tool === 'lsp' ? args.operation : tool === 'ios_simulator' ? args.action : '';
  if (action && hasOwn(toolTranscriptEnUS.actions, action)) label = t(`toolTranscript.actions.${action}`);
  const primary = primaryObject(tool, args, entry, t);
  const pathObject = FILE_TOOLS.has(tool) || primary.path || PATH_KEYS.has(primary.key);
  const fullObject = primary.value;
  let detail = pathObject ? (primary.key === 'image_paths'
    ? (Array.isArray(args.image_paths) ? args.image_paths : [args.image_paths]).map(basenameForPath).join(', ')
    : basenameForPath(fullObject)) : fullObject;
  if (primary.key === 'status') detail = t(`toolTranscript.values.${detail}`, { defaultValue: detail });
  detail = detail ? compactOneLinePreview(detail, tool === 'bash' ? 80 : 120) : '';
  const fields = [];
  const seen = new Set();
  const add = (key, value, options = {}) => {
    if (value === '' || value == null || seen.has(key)) return;
    if (SENSITIVE_KEY.test(key) && key !== 'token_budget') return;
    const raw = textValue(value);
    if (!raw) return;
    seen.add(key);
    const localized = ENUM_FIELDS.has(key) && hasOwn(toolTranscriptEnUS.values, raw)
      ? t(`toolTranscript.values.${raw}`) : raw;
    fields.push({ key, label: key === '+' || key === '-' ? key
      : t(`toolTranscript.fields.${key}`, { defaultValue: key.replace(/_/g, ' ') }),
      value: localized, ...options });
  };
  if (pathObject && fullObject) add('path', fullObject, { standalone: true });
  // A shortened query/prompt is recoverable in details; short primary objects
  // are already readable in the row. Shell handles actual layout truncation.
  if (!pathObject && tool !== 'bash' && fullObject && detail !== fullObject) {
    add(ARG_LABELS[primary.key] || primary.key || 'target', fullObject);
  }
  if (builtin) {
    for (const [key, value] of Object.entries(args)) {
      if (key === primary.key || key === 'command' || key === 'operation' || key === 'action') continue;
      if (key === 'start_line' || key === 'end_line') continue;
      const field = key === 'path' && (tool === 'grep' || tool === 'glob') ? 'scope' : (ARG_LABELS[key] || key);
      if (!hasOwn(toolTranscriptEnUS.fields, field)) continue;
      // False opt-in flags are defaults; explicit pin/archive changes still matter.
      if (value === false && !['enabled', 'pinned', 'archived'].includes(key)) continue;
      if ((key === 'path' || key === 'cwd') && value === '.') continue;
      if (field === 'limit' && value > 0) add(field, t('toolTranscript.maxResults', { count: Number(value) }), { standalone: true });
      else add(field, value, { standalone: field === 'path' });
    }
    const start = Number(args.start_line);
    const end = Number(args.end_line);
    if (start > 0 || end > 0) add('range', end > 0
      ? t('toolTranscript.lineRange', { start: start || 1, end })
      : t('toolTranscript.fromLine', { line: start }), { standalone: true });
  }
  for (const [rawKey, value] of metricPairs(entry.summary?.metrics)) {
    if (!rawKey || ['summary', 'action'].includes(rawKey)) continue;
    const key = METRIC_KEYS[rawKey] || rawKey;
    if ((key === '+' || key === '-') && Number(value) === 0) continue;
    if (value === false || value === 'false') continue;
    if (key === 'path' && String(value) === fullObject) continue;
    // Prefer the actual result metric over the requested value (e.g. quality).
    const existing = fields.findIndex(f => f.key === key);
    if (existing >= 0) { fields.splice(existing, 1); seen.delete(key); }
    add(key, value);
  }
  if (!entry.isDone) {
    if (entry.totalLines > 0) add('outputLines', entry.totalLines);
    if (entry.totalBytes > 0) add('size', formatBytes(entry.totalBytes));
  }
  if (elapsed > 0) add('elapsed', formatElapsed(elapsed));
  return {
    tool, builtin, label, detail, fullObject, fields,
    separator: t('toolTranscript.separator'),
    output: String(builtin ? (entry.resultOutput ?? entry.output ?? '') : (entry.output ?? '')),
  };
}
