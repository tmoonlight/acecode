import { tr } from '../i18n/index.js';

const isObject = (value) => value !== null && typeof value === 'object' && !Array.isArray(value);
const asText = (value) => typeof value === 'string' ? value : value == null ? '' : JSON.stringify(value, null, 2);
const compactCode = (metadata) => metadata.compact_notice_complete === true ? 'context_compacted'
  : ({ error: 'context_compact_failed', warning: 'context_compact_warning', checkpoint: 'context_checkpoint' })[metadata.compact_notice_stage]
    || 'context_compacting';

// Historical text is decoded only here. New events carry their own semantics.
const legacyMessages = new Map([
  ['Goal storage is not available.', 'goal_unavailable'],
  ['No goal set. Use /goal <objective> to create one.', 'goal_missing'],
  ['No goal to clear.', 'goal_missing'], ['No goal to resume.', 'goal_missing'], ['No goal to edit.', 'goal_missing'],
  ['No active session is available for /goal.', 'goal_no_session'],
  ['Goal cleared.', 'goal_cleared'], ['Goal paused.', 'goal_paused'], ['Goal resumed.', 'goal_resumed'],
  ['Goal is not active.', 'goal_inactive'], ['Goal is already complete.', 'goal_complete'],
  ['Goal is over its token budget. Create a replacement goal with a larger budget.', 'goal_budget_reached'],
  ['Goal token budget must be a positive integer, optionally suffixed with K or M.', 'goal_invalid_budget'],
  ['[Goal] Token budget reached; automatic continuation stopped.', 'goal_budget_reached'],
  ['[Goal] Provider usage limit hit; goal marked usage_limited and automatic continuation stopped. Use /goal resume to continue later.', 'goal_usage_limited'],
  ['[Goal] Turn ended with an error; goal marked blocked and automatic continuation stopped. Use /goal resume to retry.', 'goal_blocked'],
  ['Remote control is not running.', 'remote_control_not_running'], ['Remote control stopped.', 'remote_control_stopped'],
  ['Compacting conversation...', 'context_compacting'], ['Context compacted', 'context_compacted'],
  ['--- [Compact Checkpoint] ---', 'context_checkpoint'],
  ['[Auto-compact] Context approaching limit, compacting...', 'context_compacting'],
  ['[Compact] Stopped by hook.', 'context_compact_stopped'], ['[Auto-compact] Stopped by hook.', 'context_compact_stopped'],
  ['[Interjected]', 'turn_interjected'], ['[Interrupted]', 'turn_interrupted'],
  ['[Invoking /init - analyzing codebase and authoring AGENT.md...]', 'init_started'],
]);

function legacyGoal(text) {
  const match = text.match(/^Goal:\s*\n\s*objective:\s*([\s\S]*?)\n\s*status:\s*(\S+)\s*\n\s*tokens:\s*(\d+)(?:\s*\/\s*(\d+)\s*\((\d+) remaining\))?\s*\n\s*elapsed:\s*(\d+)s\s*$/);
  if (!match) return null;
  return {
    objective: match[1], status: match[2], tokens_used: match[3],
    ...(match[4] ? { token_budget: match[4], remaining_tokens: match[5] } : {}),
    time_used_seconds: match[6],
  };
}

function legacyNotice(content, metadata) {
  const text = content.trim();
  const goal = legacyGoal(text);
  if (goal) return { code: 'goal_overview', params: { goal }, translated: true };
  const audit = text.match(/^\[Goal\] (Started|Resumed|Continuing): ([\s\S]+)$/);
  if (audit) return {
    code: ({ Started: 'goal_started', Resumed: 'goal_resumed', Continuing: 'goal_continuing' })[audit[1]],
    params: { objective: audit[2] }, translated: true,
  };
  const simple = legacyMessages.get(text);
  if (simple) return { code: simple, params: {}, translated: true };
  const stop = text.match(/^(Remote control is not running\.|Remote control stopped\.)\nChannel deactivate warning: ([\s\S]*)$/);
  if (stop) return { code: legacyMessages.get(stop[1]), params: { warning: stop[2] }, translated: true };
  const goalError = text.match(/^Goal error: ([\s\S]+)$/);
  if (goalError) return { code: 'goal_error', params: { error: goalError[1] }, translated: true };
  if (metadata.turn_interrupt === true) return { code: 'turn_interjected', params: {} };
  if (metadata.user_aborted === true) return { code: 'turn_interrupted', params: {} };
  if (metadata.compact_notice === true) return { code: compactCode(metadata), params: {} };
  if (/^\[Conversation summary\]\r?\n/.test(text)) return { code: 'context_compacted', params: {} };
  if (/^Remote control\s*:/.test(text)) return { code: 'remote_control_status', params: {} };
  if (/^Channel '.+' connected\./.test(text)) return { code: 'remote_control_connected', params: {} };
  if (/^\[Auto-compact(?: failed)?\]/.test(text)) return { code: 'context_compact_failed', params: {} };
  if (/^\[智能压缩\]/.test(text)) return { code: 'context_compacting', params: {} };
  if (/^\[空回复\]/.test(text)) return { code: 'response_empty_retry', params: {} };
  if (/^\[输出截断\] 本回复因输出 token 上限被截断,内容可能不完整。$/.test(text)) return { code: 'response_truncated', params: {}, translated: true };
  if (/^\[Hook\]/.test(text)) return { code: 'hook_message', params: { text: text.replace(/^\[Hook\]\s*/, '') }, translated: true };
  const session = text.match(/^Continued in session (.+)\.$/);
  if (session) return { code: 'session_continued', params: { session: session[1] }, translated: true };
  const iterations = text.match(/^Agent loop stopped: reached max_iterations \((\d+)\)$/);
  if (iterations) return { code: 'iteration_limit', params: { limit: iterations[1] }, translated: true };
  const plan = text.match(/^Plan mode enabled\.\n(?:Plan file: (.*)\n)?Explore and update only the plan file, then call ExitPlanMode for approval\.$/);
  if (plan) return { code: 'plan_enabled', params: { path: plan[1] || '' }, translated: true };
  return { code: '', params: {} };
}

export function decodeSystemNotice({ content, metadata } = {}) {
  const text = asText(content);
  const meta = isObject(metadata) ? metadata : {};
  const notice = meta.system_notice;
  if (isObject(notice) && notice.version === 1 && typeof notice.code === 'string' && notice.code) {
    return { code: notice.code, params: isObject(notice.params) ? notice.params : {}, translated: true, text };
  }
  if (notice != null) return { code: '', params: {}, text, opaque: true };
  return { ...legacyNotice(text, meta), text };
}

function goalDetails(goal, t) {
  // Do not discard future fields: unknown keys remain visible with their value.
  const order = ['objective', 'status', 'tokens_used', 'token_budget', 'remaining_tokens', 'time_used_seconds', 'goal_id', 'thread_id', 'created_at_ms', 'updated_at_ms'];
  const keys = [...order.filter((key) => Object.hasOwn(goal, key)), ...Object.keys(goal).filter((key) => !order.includes(key))];
  return keys.map((key) => {
    const value = goal[key];
    const label = t(`systemNotice.fields.${key}`, { defaultValue: key });
    let rendered = key === 'status' ? t(`systemNotice.states.${value}`, { defaultValue: asText(value) })
      : value === null && (key === 'token_budget' || key === 'remaining_tokens') ? t('systemNotice.unlimited') : asText(value);
    if (['created_at_ms', 'updated_at_ms'].includes(key) && typeof value === 'number' && Number.isFinite(new Date(value).getTime())) rendered = new Date(value).toISOString();
    return `${label}: ${rendered}`;
  }).join('\n');
}

function legacyDetails(text, t) {
  // Translate known formatting, never trim or truncate unrecognized log text.
  return text.split('\n').map((line) => {
    const trimmed = line.trim();
    const code = legacyMessages.get(trimmed);
    if (code) return t(`systemNotice.details.${code}`, { defaultValue: t(`systemNotice.titles.${code}`) });
    if (trimmed === '[Conversation summary]') return `${t('systemNotice.fields.summary')}:`;
    if (trimmed === '[Auto-compact] provider unavailable for compaction') return t('systemNotice.providerUnavailable');
    if (trimmed === 'Run /rc in a session to bind it to the default channel.') return t('systemNotice.remoteHint');
    const fields = { 'Default channel': 'defaultChannel', 'Active channel': 'activeChannel', 'Bound session': 'boundSession', Inbound: 'inbound', 'Token header': 'tokenHeader', Outbound: 'outbound', Stats: 'stats' };
    const field = line.match(/^\s*([^:]+?)\s*:\s*(.*)$/);
    if (field && fields[field[1]]) {
      let value = field[2];
      if (value === '(not configured)') value = t('systemNotice.remoteUnconfigured');
      const stats = value.match(/^in (\d+) ok \/ (\d+) rejected \| out (\d+) sent \/ (\d+) failed \/ (\d+) dropped$/);
      if (stats) value = t('systemNotice.remoteStats', { accepted: stats[1], rejected: stats[2], sent: stats[3], failed: stats[4], dropped: stats[5] });
      return `${t(`systemNotice.fields.${fields[field[1]]}`)}: ${value}`;
    }
    const status = trimmed.match(/^Remote control\s*: (ON|OFF)$/);
    if (status) return `${t('systemNotice.titles.remote_control_status')}: ${t(status[1] === 'ON' ? 'systemNotice.remoteOn' : 'systemNotice.remoteOff')}`;
    const connected = trimmed.match(/^Channel '(.+)' connected\.( Existing runtime reused\.)?$/);
    if (connected) return t('systemNotice.remoteConnected', { channel: connected[1] }) + (connected[2] ? ` ${t('systemNotice.remoteReused')}` : '');
    const bound = trimmed.match(/^This session is now bound to the channel(?: \(replaced session (.+)\))?\.$/);
    if (bound) return t(bound[1] ? 'systemNotice.remoteReplaced' : 'systemNotice.remoteBound', { session: bound[1] });
    return line;
  }).join('\n');
}

export function presentSystemNotice(message = {}, t = tr) {
  const { code, params, translated, text, opaque } = decodeSystemNotice(message);
  const roleCode = message.role === 'tool_call' ? 'tool_call' : ['tool', 'tool_result'].includes(message.role) ? 'tool_result' : '';
  // 标题只插值 agent 路径(蜂群模式（网状）的 agent 间消息):其它参数可能带 count 之类
  // 会改变 i18next 取词规则的键,不往标题里传。
  const titleValues = translated && params.sender != null
    ? { sender: asText(params.sender), recipient: asText(params.recipient) }
    : {};
  const localizedTitle = t(`systemNotice.titles.${code || roleCode}`, { ...titleValues, defaultValue: '' });
  const custom = message.metadata?.compact_label;
  const fallback = custom && custom !== 'Context compacted' ? asText(custom) : text.trim().split(/\r?\n/)[0];
  const title = localizedTitle || (fallback ? Array.from(fallback).slice(0, 80).join('') : t('systemNotice.titles.notice'));
  let details = opaque || (code && !localizedTitle) ? text : legacyDetails(text, t);
  if (translated && localizedTitle) {
    const values = { ...params, reason: t(params.truncated ? 'systemNotice.truncatedResponse' : 'systemNotice.emptyResponse') };
    const template = t(`systemNotice.details.${code}`, { ...values, defaultValue: '' });
    if (isObject(params.goal)) details = [template, goalDetails(params.goal, t)].filter(Boolean).join('\n\n');
    else if (typeof params.objective === 'string') details = goalDetails({ objective: params.objective }, t);
    else if (typeof params.summary === 'string') details = `${t('systemNotice.fields.summary')}:\n${params.summary}`;
    else if (typeof params.text === 'string') details = params.text;
    else if (params.provider_unavailable === true) details = t('systemNotice.providerUnavailable');
    else if (code === 'context_compact_warning' && params.groups != null) details = t(params.thinned ? 'systemNotice.compactFallbackThinned' : 'systemNotice.compactFallback', params);
    else if (template) details = template;
    else if (typeof params.error === 'string') details = title;
    if (typeof params.error === 'string' && params.error) details += `\n\n${t('systemNotice.fields.error')}:\n${params.error}`;
    if (typeof params.warning === 'string' && params.warning) details += `\n\n${t('systemNotice.fields.warning')}:\n${params.warning}`;
    if (Array.isArray(params.entries)) {
      details = params.entries.map((entry) => presentSystemNotice(entry, t).text).join('\n\n');
    }
  }
  return { code, title, text: details };
}

// Merge only the old, adjacent summary + audit pair for the same objective.
// New producers already emit a single notice. Other messages stay separate.
export function mergeLegacyGoalNotices(items) {
  const result = [];
  for (const item of items) {
    const previous = result.at(-1);
    if (item?.kind === 'msg' && item.role === 'system' && previous?.kind === 'msg' && previous.role === 'system'
        && !item.metadata?.system_notice && !previous.metadata?.system_notice) {
      const audit = decodeSystemNotice(item);
      const overview = decodeSystemNotice(previous);
      if (audit.code === 'goal_started' && overview.code === 'goal_overview'
          && audit.params.objective === overview.params.goal.objective) {
        result[result.length - 1] = {
          ...item,
          content: `${previous.content}\n\n${item.content}`,
          metadata: { ...item.metadata, system_notice: { version: 1, code: 'goal_started', params: overview.params } },
          coveredItemIds: [previous.id, item.id],
        };
        continue;
      }
    }
    result.push(item);
  }
  return result;
}
