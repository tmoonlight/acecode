import { useCallback, useEffect, useMemo, useRef, useSyncExternalStore } from 'react';
import { createApi } from './api.js';
import { ensureSessionOpen, reportSessionOpen } from './sessionOpenDiagnostics.js';
import { connection } from './connection.js';
import { attachmentsFromContentParts, normalizeAttachmentList } from './messageAttachments.js';
import { sessionDisplayTitle } from './sessionTitle.js';
import { transcriptTimestampMs } from './timestamps.js';
import { fallbackToolSummary } from './toolSummaryFallback.js';
import { normalizeToolInvocationItems } from './transcriptProjection.js';
import { createSingleWriterStore } from './singleWriterStore.js';
import { composerContentFromMessage } from './composerContent.js';
import { preambleFromProgress, preambleFromToolStart, stripTextPreambleTags } from './toolPreamble.js';

function composerContentFields(message, fallback = null) {
  const content = composerContentFromMessage(message) || composerContentFromMessage(fallback);
  return content ? { composerContent: content } : {};
}

export function messageKey(role, content) {
  return `${role || ''}\u0000${content || ''}`;
}

function messageEventUsesOccurrenceIdentity(payload) {
  return (payload?.role || 'system') === 'error';
}

// 回放(REST since=N 补拉 / WS 带游标订阅)时 token / reasoning 先攒着,直到看见
// 能决定它们归属的事件再 flush。旧写法是「遇到任何非流式事件就 flush」,但 daemon
// 在最后一个 token 与 assistant message 事件之间恒有一条 usage(chat_stream 一返回
// 就 emit,message 要到 execute_tool_calls / 文本回合收尾才发;工具回合还会夹着
// tool_planning 的 agent_progress):usage 把 token 提前 flush 成一条新草稿,随后
// 那条 REST 历史里已有的 message 事件再把草稿「定稿」成第二份 —— 切进正在运行的
// 会话(busy → since=1 全量回放)时,上一轮的 assistant 正文就在用户消息后面又
// 出现一次。只有会往 transcript 追加 / 收尾条目的事件才需要先 flush;下面这些
// 只改 usage / activity / goal / todo 等旁路状态,与草稿的先后无关。
const STREAM_NEUTRAL_EVENT_TYPES = new Set([
  'usage',
  'agent_progress',
  'model_step_start',
  'model_step_finish',
  'goal_updated',
  'goal_cleared',
  'todo_updated',
  'session_updated',
  'permission_request',
  'permission_closed',
  'question_request',
  'question_closed',
]);

function eventFlushesPendingStream(ev) {
  return !STREAM_NEUTRAL_EVENT_TYPES.has(ev?.type || '');
}

// 攒着的 token 入队时已经过了水位检查;推迟到 flush 时,水位可能已被中间的旁路
// 事件(usage 等)推高,不能再把它们当过期帧丢掉 —— 否则被中断的回合(没有
// assistant message 收尾)已流出的正文会整段消失。应用后水位仍取较大值。
function reduceDeferredStreamEvent(state, ev) {
  const watermark = Number(state?.lastSeq) || 0;
  const seq = eventSeq(ev);
  if (seq === null || seq > watermark) return reduceTranscriptEvent(state, ev);
  const reduced = reduceTranscriptEvent({ ...state, lastSeq: seq - 1 }, ev);
  reduced.state.lastSeq = Math.max(watermark, Number(reduced.state.lastSeq) || 0);
  return reduced;
}

// message 事件命中已有条目时的原位更新:事件可能带更完整的 content_parts / metadata。
function mergeMessageEventIntoItem(item, payload, msg) {
  const incomingContent = payload.content || '';
  return {
    ...item,
    role: payload.role || 'system',
    content: incomingContent || item.content || '',
    contentParts: Array.isArray(payload.content_parts) ? payload.content_parts : item.contentParts,
    metadata: payload.metadata ?? item.metadata,
    ...composerContentFields(payload, item),
    ts: eventTs(msg),
  };
}

// 回放帧里的 assistant message 若已在 transcript 里(REST 历史已含这条消息),
// 说明此前回放出来的流式草稿只是它的副本:草稿必须丢弃,而不是被「定稿」成第二份。
// 只对 replayed 帧生效 —— assistant 消息 id 是内容哈希,实时回合里模型说了一句
// 与旧消息一字不差的话时 id 也相同,那是真的新回复,必须照常展示。
function replayedDuplicateMessageIndex(items, payload, msg, isDraft) {
  if (msg?.replayed !== true) return -1;
  const incomingId = payload?.id || '';
  if (!incomingId || messageEventUsesOccurrenceIdentity(payload)) return -1;
  return items.findIndex((item, index) => (
    !isDraft(item, index) && item.kind === 'msg' && item.messageId === incomingId
  ));
}

function dropDraftAndMergeReplayedMessage(items, duplicateIndex, isDraft, payload, msg) {
  return items
    .map((item, index) => (index === duplicateIndex
      ? mergeMessageEventIntoItem(item, payload, msg)
      : item))
    .filter((item, index) => !isDraft(item, index));
}

// 同一根因的工具版:REST 历史里已有这次调用落盘的结果条目(本回合里已经跑完的
// 工具,含答完的 AskUserQuestion 卡片)时,回放的 tool_start 不能再追加一条,
// 而是把后续 tool_update / tool_end 绑到已有条目上。从尾部找最近一条未被
// 绑定的同 call id 条目 —— 有的 provider 会跨回合复用 call id。同样只对
// replayed 帧生效:实时回合里复用的 call id 是真的新调用。
function replayedPersistedToolIndex(state, toolCallId, msg) {
  if (msg?.replayed !== true || !toolCallId) return -1;
  const bound = new Set(state.toolMap.values());
  for (let index = state.items.length - 1; index >= 0; index -= 1) {
    const item = state.items[index];
    if (item?.kind !== 'tool' || item.tool?.isDone !== true || bound.has(item.id)) continue;
    if (String(item.tool?.toolCallId || '') === String(toolCallId)) return index;
  }
  return -1;
}

function normalizeSessionRef(sessionRef) {
  if (!sessionRef) return null;
  if (typeof sessionRef === 'string') return { sessionId: sessionRef };
  const sessionId = sessionRef.sessionId || sessionRef.id || '';
  if (!sessionId) return null;
  return {
    ...sessionRef,
    sessionId,
    workspaceHash: sessionRef.workspaceHash || sessionRef.workspace_hash || '',
  };
}

// 从侧栏 / 跳转目标的会话对象里抄出显示标题的三个服务端字段,作为 store 的初值;
// 之后由 session_updated 与 messages 快照刷新。
function sessionTitleFieldsFromRef(ref) {
  const s = ref && typeof ref === 'object' ? ref : {};
  return {
    title: typeof s.title === 'string' ? s.title : '',
    titleSource: String(s.title_source ?? s.titleSource ?? ''),
    summary: typeof s.summary === 'string' ? s.summary : '',
  };
}

// 顶部标题 = 侧栏同一条规则(sessionDisplayTitle):title > summary > 计数兜底。
// ref 仍提供 message_count / displayTitle(「新会话N」)等兜底字段。
export function transcriptDisplayTitle(state, ref) {
  const base = ref && typeof ref === 'object' ? ref : {};
  if (!state) return sessionDisplayTitle(base);
  return sessionDisplayTitle({
    ...base,
    title: state.title || '',
    title_source: state.titleSource || '',
    summary: state.summary || '',
  });
}

function cloneToolMap(toolMap) {
  if (toolMap instanceof Map) return new Map(toolMap);
  if (toolMap && typeof toolMap === 'object') return new Map(Object.entries(toolMap));
  return new Map();
}

function cloneTurnTimings(turnTimings) {
  const out = new Map();
  const entries = turnTimings instanceof Map
    ? turnTimings.entries()
    : Object.entries(turnTimings && typeof turnTimings === 'object' ? turnTimings : {});
  for (const [key, value] of entries) {
    if (!key || !value || typeof value !== 'object') continue;
    out.set(String(key), { ...value });
  }
  return out;
}

function cloneTurnNetDiffs(turnNetDiffs) {
  const out = new Map();
  const entries = turnNetDiffs instanceof Map
    ? turnNetDiffs.entries()
    : Object.entries(turnNetDiffs && typeof turnNetDiffs === 'object' ? turnNetDiffs : {});
  for (const [key, value] of entries) {
    if (!key || !value || typeof value !== 'object') continue;
    out.set(String(key), cloneJsonLike(value));
  }
  return out;
}

function cloneTokenUsage(tokenUsage) {
  if (!tokenUsage || typeof tokenUsage !== 'object') return null;
  const cloned = { ...tokenUsage };
  if (tokenUsage.contextBreakdown && typeof tokenUsage.contextBreakdown === 'object') {
    cloned.contextBreakdown = { ...tokenUsage.contextBreakdown };
  }
  return cloned;
}

function cloneGoal(goal) {
  if (!goal || typeof goal !== 'object') return null;
  return { ...goal };
}

function cloneJsonLike(value) {
  if (value == null) return null;
  if (Array.isArray(value)) return value.map((item) => cloneJsonLike(item));
  if (typeof value === 'object') {
    const out = {};
    for (const [key, item] of Object.entries(value)) out[key] = cloneJsonLike(item);
    return out;
  }
  return value;
}

function normalizeTodoStatus(status) {
  const value = String(status || '').trim().toLowerCase();
  if (value === 'pending' || value === 'in_progress' || value === 'completed' || value === 'cancelled') return value;
  return 'pending';
}

function normalizeTodos(todos) {
  if (!Array.isArray(todos)) return [];
  return todos
    .filter((item) => item && typeof item === 'object' && !Array.isArray(item))
    .map((item) => ({
      id: String(item.id ?? '').trim() || '?',
      content: String(item.content ?? '').trim() || '(no description)',
      status: normalizeTodoStatus(item.status),
    }));
}

function cloneTodos(todos) {
  return normalizeTodos(todos);
}

function normalizeTodoSummary(summary, todos = []) {
  const fallback = { total: todos.length, pending: 0, in_progress: 0, completed: 0, cancelled: 0 };
  for (const item of todos) {
    fallback[item.status] = (fallback[item.status] || 0) + 1;
  }
  if (!summary || typeof summary !== 'object') return fallback;
  return {
    total: Math.max(0, Number(summary.total) || fallback.total),
    pending: Math.max(0, Number(summary.pending) || 0),
    in_progress: Math.max(0, Number(summary.in_progress ?? summary.inProgress) || 0),
    completed: Math.max(0, Number(summary.completed) || 0),
    cancelled: Math.max(0, Number(summary.cancelled) || 0),
  };
}

function cloneState(state) {
  return {
    ...state,
    items: Array.isArray(state.items) ? state.items : [],
    toolMap: cloneToolMap(state.toolMap),
    turnTimings: cloneTurnTimings(state.turnTimings),
    turnNetDiffs: cloneTurnNetDiffs(state.turnNetDiffs),
    tokenUsage: cloneTokenUsage(state.tokenUsage),
    goal: cloneGoal(state.goal),
    todos: cloneTodos(state.todos),
    todoSummary: state.todoSummary && typeof state.todoSummary === 'object' ? { ...state.todoSummary } : null,
    activity: state.activity && typeof state.activity === 'object' ? { ...state.activity } : null,
    trajectoryPartial: state.trajectoryPartial && typeof state.trajectoryPartial === 'object'
      ? {
          ...state.trajectoryPartial,
          blocks: Array.isArray(state.trajectoryPartial.blocks)
            ? state.trajectoryPartial.blocks.map((block) => ({ ...block }))
            : [],
        }
      : null,
  };
}

function hasTrajectoryPartialWork(state) {
  const partial = state?.trajectoryPartial;
  if (!partial || typeof partial !== 'object') return false;
  if (partial.step != null) return true;
  return Array.isArray(partial.blocks) && partial.blocks.length > 0;
}

export function hasInFlightTranscriptWork(state = {}) {
  return !!state?.activeTurnId
    || state?.streamingId != null
    || hasTrajectoryPartialWork(state)
    || !!state?.activity;
}

export function isTranscriptActivelyRunning(state = {}) {
  return state?.busy === true
    || state?.status === 'running'
    || hasInFlightTranscriptWork(state);
}

function markTranscriptRunning(next, turnId = '') {
  if (!next || next.status === 'error') return next;
  next.busy = true;
  next.status = 'running';
  if (turnId && !next.activeTurnId) next.activeTurnId = String(turnId);
  return next;
}

function appendTrajectoryPartialText(next, kind, text) {
  if (!text) return;
  const current = next.trajectoryPartial && typeof next.trajectoryPartial === 'object'
    ? next.trajectoryPartial
    : { step: null, blocks: [] };
  const blocks = Array.isArray(current.blocks)
    ? current.blocks.map((block) => ({ ...block }))
    : [];
  const last = blocks[blocks.length - 1];
  if (last?.kind === kind) {
    blocks[blocks.length - 1] = { ...last, text: `${last.text || ''}${text}` };
  } else {
    blocks.push({ kind, text });
  }
  next.trajectoryPartial = { ...current, blocks };
}

function readUsageInt(payload, snakeKey, camelKey) {
  const raw = payload?.[snakeKey] ?? payload?.[camelKey];
  const value = Number(raw);
  if (!Number.isFinite(value)) return 0;
  return Math.max(0, Math.trunc(value));
}

function normalizeContextBreakdown(payload) {
  const raw = payload?.context_breakdown ?? payload?.contextBreakdown;
  if (!raw || typeof raw !== 'object') return null;
  const hasData = raw.has_data === true || raw.hasData === true;
  if (!hasData) return null;
  return {
    systemPrompt: readUsageInt(raw, 'system_prompt', 'systemPrompt'),
    projectRules: readUsageInt(raw, 'project_rules', 'projectRules'),
    skills: readUsageInt(raw, 'skills', 'skills'),
    builtinTools: readUsageInt(raw, 'builtin_tools', 'builtinTools'),
    mcpTools: readUsageInt(raw, 'mcp_tools', 'mcpTools'),
    conversation: readUsageInt(raw, 'conversation', 'conversation'),
    dynamicContext: readUsageInt(raw, 'dynamic_context', 'dynamicContext'),
    hasData: true,
  };
}

function normalizeUsagePayload(payload, timestampMs) {
  const hasDataRaw = payload?.has_data ?? payload?.hasData;
  const normalized = {
    promptTokens: readUsageInt(payload, 'prompt_tokens', 'promptTokens'),
    completionTokens: readUsageInt(payload, 'completion_tokens', 'completionTokens'),
    totalTokens: readUsageInt(payload, 'total_tokens', 'totalTokens'),
    cacheReadTokens: readUsageInt(payload, 'cache_read_tokens', 'cacheReadTokens'),
    cacheWriteTokens: readUsageInt(payload, 'cache_write_tokens', 'cacheWriteTokens'),
    hasData: hasDataRaw === true,
    timestampMs: Number(timestampMs) || Date.now(),
  };
  const contextBreakdown = normalizeContextBreakdown(payload);
  if (contextBreakdown) normalized.contextBreakdown = contextBreakdown;
  return normalized;
}

function allocateItemId(state) {
  const id = state.nextItemId || 1;
  state.nextItemId = id + 1;
  return id;
}

function eventTs(msg) {
  return transcriptTimestampMs(msg) || Date.now();
}

function clientMessageIdFromMetadata(metadata) {
  if (!metadata || typeof metadata !== 'object' || Array.isArray(metadata)) return '';
  return typeof metadata.client_message_id === 'string'
    ? metadata.client_message_id.trim()
    : '';
}

function eventSeq(msg) {
  return typeof msg?.seq === 'number' && Number.isFinite(msg.seq) ? msg.seq : null;
}

function isStaleSequencedEvent(state, msg) {
  const seq = eventSeq(msg);
  return seq !== null && seq <= (state?.lastSeq || 0);
}

function markEventSeqApplied(state, msg) {
  const seq = eventSeq(msg);
  if (seq !== null && seq > (state.lastSeq || 0)) {
    state.lastSeq = seq;
  }
}

function terminationNoticeText(payload = {}) {
  const source = payload.source || '';
  const reason = String(payload.reason || payload.message || '').trim();
  if (source === 'user') return reason || '用户已终止本轮任务';
  return reason ? `任务已终止：${reason}` : '任务已终止';
}

function isAbortLikeReason(reason) {
  return /abort|cancel|interrupt|terminat|用户.*终止|已终止|取消|中断/i.test(String(reason || ''));
}

function isUserAbortMessage(message) {
  return message?.role === 'system' && message.metadata?.transcript_only === true &&
    message.metadata?.user_aborted === true;
}

function appendTerminationNotice(next, msg, payload = {}) {
  const text = terminationNoticeText(payload);
  const last = next.items[next.items.length - 1];
  if (last?.kind === 'termination_notice') {
    if (last.content === text) {
      if (payload.metadata?.user_aborted === true) {
        next.items = [...next.items.slice(0, -1), {
          ...last, messageId: payload.id || '', metadata: payload.metadata,
        }];
      }
      return;
    }
    if (last.source === 'user' && payload.source !== 'user' && isAbortLikeReason(payload.reason || payload.message)) {
      return;
    }
  }
  next.items = [
    ...next.items,
    {
      kind: 'termination_notice',
      id: allocateItemId(next),
      source: payload.source || 'server',
      ...(payload.metadata ? { metadata: payload.metadata, messageId: payload.id || '' } : {}),
      content: text,
      ts: eventTs(msg),
    },
  ];
}

function normalizeSummaryMetrics(metrics) {
  if (!Array.isArray(metrics)) return [];
  return metrics
    .map((metric) => {
      if (Array.isArray(metric) && metric.length >= 2) {
        return { label: String(metric[0] ?? ''), value: String(metric[1] ?? '') };
      }
      if (metric && typeof metric === 'object') {
        return {
          label: String(metric.label ?? ''),
          value: String(metric.value ?? ''),
        };
      }
      return null;
    })
    .filter((metric) => metric && metric.label);
}

function normalizePersistedToolSummary(metadata) {
  const raw = metadata?.tool_summary;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  return {
    verb: typeof raw.verb === 'string' ? raw.verb : '',
    object: typeof raw.object === 'string' ? raw.object : '',
    icon: typeof raw.icon === 'string' ? raw.icon : '',
    metrics: normalizeSummaryMetrics(raw.metrics),
  };
}

function normalizeAskUserQuestionResult(metadata) {
  const raw = metadata?.ask_user_question_result ?? metadata?.askUserQuestionResult;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  // cancelled=true 是「用户拒绝作答」的落盘标记。即使没有回答项,也要在
  // 消息流里恢复成 tool item,否则「已取消全部回答」卡无从锚定、无法持久展示。
  const cancelled = raw.cancelled === true;
  // interjected=true 是「用户改为直接输入」(插话取消作答)的落盘标记,同理要
  // 恢复成 tool item,否则历史页里那条问题看起来像是没有下文。
  const interjected = raw.interjected === true;
  const items = Array.isArray(raw.items) ? raw.items : [];
  const normalized = items
    .filter((item) => item && typeof item === 'object' && !Array.isArray(item))
    .map((item) => ({
      question: String(item.question ?? item.q ?? ''),
      answer: String(item.answer ?? item.a ?? ''),
      multiSelect: item.multi_select === true || item.multiSelect === true,
    }))
    .filter((item) => item.question || item.answer);
  if (cancelled) return { cancelled: true, items: normalized };
  if (interjected) return { interjected: true, items: normalized };
  return normalized.length > 0 ? { items: normalized } : null;
}

function readRuntimeTurnCount(data) {
  const raw = data?.turn_count ?? data?.turnCount;
  const value = Number(raw);
  if (!Number.isFinite(value)) return null;
  return Math.max(0, Math.trunc(value));
}

function normalizeTurnTimingRecord(message) {
  const metadata = message?.metadata;
  const raw = metadata && typeof metadata === 'object' && !Array.isArray(metadata)
    ? metadata.turn_timing
    : null;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  const userMessageUuid = String(raw.user_message_uuid || raw.userMessageUuid || '').trim();
  if (!userMessageUuid) return null;
  const startedAtMs = Number(raw.started_at_ms ?? raw.startedAtMs);
  const completedAtMs = Number(raw.completed_at_ms ?? raw.completedAtMs);
  const durationMs = Number(raw.duration_ms ?? raw.durationMs);
  const status = String(raw.status || '').trim();
  return {
    userMessageUuid,
    startedAtMs: Number.isFinite(startedAtMs) ? Math.max(0, Math.trunc(startedAtMs)) : 0,
    completedAtMs: Number.isFinite(completedAtMs) ? Math.max(0, Math.trunc(completedAtMs)) : 0,
    durationMs: Number.isFinite(durationMs) ? Math.max(0, Math.trunc(durationMs)) : 0,
    status,
  };
}

function nonNegativeInteger(value) {
  return Number.isSafeInteger(value) && value >= 0 ? value : null;
}

function normalizeTurnDiffLine(line) {
  if (!line || typeof line !== 'object' || Array.isArray(line)) return null;
  if (!['context', 'added', 'removed'].includes(line.kind) || typeof line.text !== 'string') {
    return null;
  }
  const normalized = { kind: line.kind, text: line.text };
  for (const key of ['old_line_no', 'new_line_no']) {
    if (!Object.prototype.hasOwnProperty.call(line, key)) continue;
    const value = nonNegativeInteger(line[key]);
    if (value === null) return null;
    normalized[key] = value;
  }
  return normalized;
}

function normalizeTurnDiffHunk(hunk) {
  if (!hunk || typeof hunk !== 'object' || Array.isArray(hunk) || !Array.isArray(hunk.lines)) {
    return null;
  }
  const normalized = { lines: [] };
  for (const key of ['old_start', 'old_count', 'new_start', 'new_count']) {
    const value = nonNegativeInteger(hunk[key]);
    if (value === null) return null;
    normalized[key] = value;
  }
  for (const line of hunk.lines) {
    const normalizedLine = normalizeTurnDiffLine(line);
    if (!normalizedLine) return null;
    normalized.lines.push(normalizedLine);
  }
  return normalized;
}

export function normalizeTurnNetDiffRecord(messageOrPayload) {
  const metadata = messageOrPayload?.metadata;
  const raw = metadata && typeof metadata === 'object' && !Array.isArray(metadata)
    ? metadata.turn_net_diff
    : (messageOrPayload?.turn_net_diff ?? messageOrPayload);
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) return null;
  if (typeof raw.user_message_uuid !== 'string' || !raw.user_message_uuid.trim()) return null;
  if (typeof raw.complete !== 'boolean' || !Array.isArray(raw.files) || !Array.isArray(raw.errors)) {
    return null;
  }

  const errors = [];
  for (const error of raw.errors) {
    if (typeof error !== 'string') return null;
    errors.push(error);
  }

  const files = [];
  for (const file of raw.files) {
    if (!file || typeof file !== 'object' || Array.isArray(file) ||
        typeof file.file !== 'string' || !file.file.trim() || !Array.isArray(file.hunks)) {
      return null;
    }
    const additions = nonNegativeInteger(file.additions);
    const deletions = nonNegativeInteger(file.deletions);
    if (additions === null || deletions === null) return null;
    const hunks = [];
    for (const hunk of file.hunks) {
      const normalizedHunk = normalizeTurnDiffHunk(hunk);
      if (!normalizedHunk) return null;
      hunks.push(normalizedHunk);
    }
    files.push({ file: file.file.trim(), additions, deletions, hunks });
  }

  return {
    userMessageUuid: raw.user_message_uuid.trim(),
    complete: raw.complete,
    files,
    errors,
  };
}

function normalizePersistedToolHunks(metadata) {
  const raw = metadata?.tool_hunks;
  if (!Array.isArray(raw)) return [];
  return raw
    .filter((hunk) => hunk && typeof hunk === 'object' && !Array.isArray(hunk))
    .map((hunk) => ({ ...hunk }));
}

function stringFromPersistedValue(value) {
  if (value == null) return '';
  if (typeof value === 'string') return value;
  if (typeof value === 'number' || typeof value === 'boolean' || typeof value === 'bigint') {
    return String(value);
  }
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function persistedToolCallId(raw) {
  const id = raw?.id ?? raw?.tool_call_id ?? raw?.toolCallId ?? raw?.call_id ?? '';
  return String(id || '').trim();
}

function persistedToolCallIndex(raw, fallbackIndex) {
  const value = raw?.tool_index ?? raw?.toolIndex ?? raw?.index;
  const number = Number(value);
  if (Number.isFinite(number)) return Math.trunc(number);
  return fallbackIndex;
}

function normalizePersistedToolCall(raw, fallbackIndex) {
  const call = raw && typeof raw === 'object' && !Array.isArray(raw) ? raw : {};
  const fn = call.function && typeof call.function === 'object' && !Array.isArray(call.function)
    ? call.function
    : null;
  const rawName = fn?.name ?? call.name ?? call.tool ?? call.tool_name ?? '';
  const rawArgs = fn && Object.prototype.hasOwnProperty.call(fn, 'arguments')
    ? fn.arguments
    : call.arguments ?? call.args ?? call.input ?? '';
  return {
    name: String(rawName || '').trim(),
    argumentsText: stringFromPersistedValue(rawArgs),
    toolCallId: persistedToolCallId(call),
    toolIndex: persistedToolCallIndex(call, fallbackIndex),
  };
}

// REST 历史把工具名放在 assistant.tool_calls,把结果放在后续 role:tool
// 消息。按消息顺序只保留尚未消费的调用,避免跨回合复用 call id 时串名。
function rememberPersistedToolNames(message, namesByCallId) {
  if (message?.role !== 'assistant') return;
  const toolCalls = Array.isArray(message.tool_calls) ? message.tool_calls : [];
  for (let i = 0; i < toolCalls.length; i += 1) {
    const call = normalizePersistedToolCall(toolCalls[i], i);
    if (call.toolCallId) {
      // A new call supersedes an interrupted call with the same ID, even when
      // its name is missing. Never let it inherit the earlier call's identity.
      if (call.name) namesByCallId.set(call.toolCallId, call.name);
      else namesByCallId.delete(call.toolCallId);
    }
  }
}

function withPersistedToolName(message, namesByCallId) {
  if (message?.role !== 'tool' || message.tool || message.tool_name) return message;
  const toolCallId = String((message.tool_call_id ?? message.toolCallId ?? '') || '').trim();
  const toolName = toolCallId ? namesByCallId.get(toolCallId) : '';
  return toolName ? { ...message, tool: toolName } : message;
}

function persistedToolCallMessageId(message, messageIndex, call) {
  const parentId = String(message?.id || `message-${messageIndex}`);
  const suffix = call.toolCallId || `index-${call.toolIndex}`;
  return `${parentId}:tool_call:${suffix}`;
}

function persistedToolCallContent(call) {
  return `[Tool: ${call.name}] ${call.argumentsText}`;
}

function messageToolCallFields(m) {
  const toolCallId = String((m?.tool_call_id ?? m?.toolCallId ?? '') || '').trim();
  const hasToolIndex = m?.tool_index != null || m?.toolIndex != null;
  const toolIndex = hasToolIndex ? (m.tool_index ?? m.toolIndex) : null;
  return { toolCallId, hasToolIndex, toolIndex };
}

function messageOrdinalValue(value, fallback = null) {
  if (value !== undefined && value !== null && value !== '') {
    const n = Number(value);
    if (Number.isInteger(n) && n >= 0) return n;
  }
  if (fallback !== undefined && fallback !== null && fallback !== '') {
    const f = Number(fallback);
    if (Number.isInteger(f) && f >= 0) return f;
  }
  return null;
}

function messageOrdinal(m, fallback = null) {
  return messageOrdinalValue(m?.__messageOrdinal ?? m?.message_ordinal ?? m?.messageOrdinal, fallback);
}

function genericHistoryMessageItem(next, m, extra = {}) {
  const fields = messageToolCallFields(m);
  const item = {
    kind: 'msg',
    id: allocateItemId(next),
    messageId: extra.messageId || m?.id || '',
    role: extra.role || m?.role || 'system',
    content: extra.content ?? m?.content ?? '',
    contentParts: extra.contentParts || (Array.isArray(m?.content_parts) ? m.content_parts : []),
    metadata: extra.metadata ?? m?.metadata,
    ...composerContentFields(extra, m),
    ts: extra.ts || transcriptTimestampMs(m) || Date.now(),
  };
  const ordinal = messageOrdinalValue(extra.messageOrdinal, messageOrdinal(m));
  if (ordinal !== null) item.messageOrdinal = ordinal;
  const toolCallId = extra.toolCallId ?? fields.toolCallId;
  const hasToolCallId = Object.prototype.hasOwnProperty.call(extra, 'toolCallId') || !!fields.toolCallId;
  const hasToolIndex = Object.prototype.hasOwnProperty.call(extra, 'toolIndex') || fields.hasToolIndex;
  const toolIndex = Object.prototype.hasOwnProperty.call(extra, 'toolIndex') ? extra.toolIndex : fields.toolIndex;
  if (hasToolCallId) {
    item.tool_call_id = toolCallId || '';
    item.toolCallId = toolCallId || '';
  }
  if (hasToolIndex) {
    item.tool_index = toolIndex;
    item.toolIndex = toolIndex;
  }
  const toolName = extra.toolName ?? m?.tool ?? m?.tool_name;
  if (toolName) item.tool_name = String(toolName);
  return item;
}

function historyItemFromMessage(next, m, messageOrdinal = null) {
  const metadata = m?.metadata && typeof m.metadata === 'object' ? m.metadata : null;
  const ts = transcriptTimestampMs(m) || Date.now();
  if (isUserAbortMessage(m)) {
    return {
      kind: 'termination_notice', id: allocateItemId(next),
      source: 'user', content: terminationNoticeText({ source: 'user' }),
      metadata, messageId: m.id || '', ts,
    };
  }
  if ((m?.role || '') === 'tool' && metadata) {
    const summary = normalizePersistedToolSummary(metadata);
    const hunks = normalizePersistedToolHunks(metadata);
    const askUserQuestionResult = normalizeAskUserQuestionResult(metadata);
    const attachments = attachmentsFromContentParts(m.content_parts);
    if (summary || hunks.length > 0 || attachments.length > 0 || askUserQuestionResult) {
      const success = typeof metadata.tool_success === 'boolean'
        ? metadata.tool_success
        : true;
      return {
        kind: 'tool',
        id: allocateItemId(next),
        messageId: m.id || '',
        tool: {
          isTaskComplete: false,
          isDone: true,
          success,
          tool: m.tool || m.tool_name || '',
          toolCallId: m.tool_call_id || m.toolCallId || '',
          toolIndex: m.tool_index ?? m.toolIndex ?? null,
          args: null,
          startedAtMs: ts,
          displayOverride: '',
          title: summary?.object || m.content || '工具调用',
          tailLines: [],
          currentPartial: '',
          totalLines: 0,
          totalBytes: 0,
          elapsed: 0,
          summary,
          output: m.content || '',
          hunks,
          attachments,
          metadata,
          askUserQuestionResult,
        },
        ts,
      };
    }
  } else if ((m?.role || '') === 'tool') {
    const attachments = attachmentsFromContentParts(m.content_parts);
    if (attachments.length > 0) {
      return {
        kind: 'tool',
        id: allocateItemId(next),
        messageId: m.id || '',
        tool: {
          isTaskComplete: false,
          isDone: true,
          success: true,
          tool: m.tool || m.tool_name || '',
          toolCallId: m.tool_call_id || m.toolCallId || '',
          toolIndex: m.tool_index ?? m.toolIndex ?? null,
          args: null,
          startedAtMs: ts,
          displayOverride: '',
          title: m.content || attachments[0]?.name || '工具调用',
          tailLines: [],
          currentPartial: '',
          totalLines: 0,
          totalBytes: 0,
          elapsed: 0,
          summary: null,
          output: m.content || '',
          hunks: [],
          attachments,
        },
        ts,
      };
    }
  }

  return genericHistoryMessageItem(next, m, { ts, messageOrdinal });
}

function historyItemsFromMessage(next, m, messageIndex) {
  const role = m?.role || '';
  const rawOrdinal = messageOrdinal(m, messageIndex);
  if (role !== 'assistant') return [historyItemFromMessage(next, m, rawOrdinal)];

  const toolCalls = Array.isArray(m?.tool_calls) ? m.tool_calls : [];
  if (toolCalls.length === 0) return [historyItemFromMessage(next, m, rawOrdinal)];

  const items = [];
  const content = m?.content ?? '';
  const contentParts = Array.isArray(m?.content_parts) ? m.content_parts : [];
  if (String(content || '').trim() || contentParts.length > 0) {
    items.push(historyItemFromMessage(next, m, rawOrdinal));
  }

  for (let i = 0; i < toolCalls.length; i += 1) {
    const call = normalizePersistedToolCall(toolCalls[i], i);
    items.push(genericHistoryMessageItem(next, m, {
      messageId: persistedToolCallMessageId(m, messageIndex, call),
      role: 'tool_call',
      content: persistedToolCallContent(call),
      contentParts: [],
      metadata: {
        ...(m?.metadata && typeof m.metadata === 'object' && !Array.isArray(m.metadata) ? m.metadata : {}),
        tool_call_id: call.toolCallId,
        tool_index: call.toolIndex,
      },
      ts: transcriptTimestampMs(m) || Date.now(),
      toolCallId: call.toolCallId,
      toolIndex: call.toolIndex,
      messageOrdinal: rawOrdinal,
    }));
  }

  return items;
}

// 工具前言(add-tool-preamble):assistant 正文里的 <text_preamble> 标签只在实时
// 期间进 loading,落盘正文保留原文(模型会模仿自己的历史输出),所以历史加载与
// message 事件都要在这里剥掉;token 流由 daemon 剥过。
function withVisibleAssistantContent(message) {
  if (message?.role !== 'assistant' || typeof message.content !== 'string') return message;
  const visible = stripTextPreambleTags(message.content);
  return visible === message.content ? message : { ...message, content: visible };
}

function historyItemsFromMessages(next, messages) {
  const items = [];
  const toolNamesByCallId = new Map();
  for (let i = 0; i < messages.length; i += 1) {
    const rawMessage = messages[i];
    if (rawMessage?.role === 'user') {
      toolNamesByCallId.clear();
    }
    const message = withVisibleAssistantContent(
      withPersistedToolName(rawMessage, toolNamesByCallId));
    items.push(...historyItemsFromMessage(next, message, i).map((item) => message.message_position == null
      ? item : { ...item, messagePosition: String(message.message_position) }));
    if (message?.role === 'tool') {
      const toolCallId = String((message.tool_call_id ?? message.toolCallId ?? '') || '').trim();
      if (toolCallId) toolNamesByCallId.delete(toolCallId);
    } else {
      rememberPersistedToolNames(message, toolNamesByCallId);
    }
  }
  return items;
}

function visibleTranscriptMessages(messages) {
  if (!Array.isArray(messages)) return [];
  return messages
    .map((m, index) => (m && typeof m === 'object' ? { ...m, __messageOrdinal: m.message_ordinal ?? m.__messageOrdinal ?? (m.message_position == null ? index : null) } : m))
    .filter((m) => !m?.is_meta && !m?.metadata?.hidden_goal_context);
}

function splitTranscriptMessages(messages) {
  const source = Array.isArray(messages) ? messages : [];
  const visible = visibleTranscriptMessages(source);
  const transcriptMessages = [];
  const turnTimings = new Map();
  const turnNetDiffs = new Map();
  for (const message of source) {
    const timing = normalizeTurnTimingRecord(message);
    if (timing) turnTimings.set(timing.userMessageUuid, timing);
    const turnNetDiff = normalizeTurnNetDiffRecord(message);
    if (turnNetDiff) turnNetDiffs.set(turnNetDiff.userMessageUuid, turnNetDiff);
  }
  for (const message of visible) {
    const timing = normalizeTurnTimingRecord(message);
    if (timing) continue;
    if (normalizeTurnNetDiffRecord(message)) continue;
    transcriptMessages.push(message);
  }
  return { messages: transcriptMessages, turnTimings, turnNetDiffs };
}

function itemUserMessageUuid(item) {
  return String(item?.messageId || item?.id || item?.metadata?.user_message_uuid || '').trim();
}

function applyTurnTimingsToItems(items, turnTimings) {
  if (!(turnTimings instanceof Map) || turnTimings.size === 0) return items;
  let activeTiming = null;
  return items.map((item) => {
    if (item?.kind === 'msg' && item.role === 'user') {
      activeTiming = turnTimings.get(itemUserMessageUuid(item)) || null;
      return item;
    }
    if (!activeTiming) return item;
    if (item?.kind !== 'msg' && item?.kind !== 'tool') return item;
    return {
      ...item,
      turnTiming: activeTiming,
      turnDurationMs: activeTiming.durationMs,
    };
  });
}

function applyTurnNetDiffsToItems(items, turnNetDiffs) {
  if (!(turnNetDiffs instanceof Map) || turnNetDiffs.size === 0) return items;
  return items.map((item) => {
    if (item?.kind !== 'msg' || item.role !== 'user') return item;
    const record = turnNetDiffs.get(itemUserMessageUuid(item));
    return record ? { ...item, turnNetDiff: record } : item;
  });
}

function applyTurnMetadataToItems(items, turnTimings, turnNetDiffs) {
  return applyTurnNetDiffsToItems(
    applyTurnTimingsToItems(items, turnTimings),
    turnNetDiffs,
  );
}

function toolKey(payload = {}) {
  if (payload.tool_call_id || payload.call_id || payload.id) {
    return payload.tool_call_id || payload.call_id || payload.id;
  }
  if (payload.tool_index !== undefined && payload.tool_index !== null) {
    return `${payload.tool || '_tool'}#${payload.tool_index}`;
  }
  return payload.tool || '_anon';
}

function finalizeStreaming(next) {
  if (next.streamingId == null) return next;
  const currentStreamingId = next.streamingId;
  next.streamingId = null;
  next.items = next.items.map((item) => item.id === currentStreamingId
    ? { ...item, streaming: false }
    : item);
  return next;
}

function trailingAssistantDraftIndex(items) {
  if (!Array.isArray(items)) return -1;
  for (let index = items.length - 1; index >= 0; index -= 1) {
    const item = items[index];
    if (!item) continue;
    if (item.kind === 'msg' && item.role === 'assistant') {
      return item.streamDraft && !item.messageId ? index : -1;
    }
    if (item.kind === 'msg' || item.kind === 'tool' || item.kind === 'termination_notice') {
      return -1;
    }
  }
  return -1;
}

function replaceAssistantItemWithFinal(item, payload, msg) {
  return {
    ...item,
    role: 'assistant',
    content: payload.content || item.content || '',
    contentParts: Array.isArray(payload.content_parts) ? payload.content_parts : item.contentParts,
    messageId: payload.id || item.messageId || '',
    metadata: payload.metadata ?? item.metadata,
    ...composerContentFields(payload, item),
    ts: eventTs(msg),
    streaming: false,
    streamDraft: false,
  };
}

// 决定 REST 历史加载后的事件补拉起点。返回 null = 不补拉。
//
// 关键成本约束:`GET messages?since=0`(初始加载)在 daemon 侧不回放任何
// 事件(EventDispatcher 只在 since>0 时回放),所以 loadedSeq 在初始加载后
// 恒为 0;此时补拉 since=1 会让 daemon 同步回放**整个 1024 条事件环形缓冲**
//(实测 feedback IQSZ-D0668 的 daemon 日志:一天内 37 次 replayed=1024),
// 前端还要逐条 reduce 消化 —— 主线程一次性烧几百毫秒到秒级,正是切会话
// 卡顿的主要成本。而对**空闲**会话这份回放是纯冗余:磁盘历史已经完整,
// 回放出来的 message 事件全部被 seenMessages 去重丢掉。
//
// 因此 loadedSeq=0 时只有 busy(有进行中的回合)才补拉:那是唯一需要从
// 事件流重建"未落盘的流式草稿"的场景。
export function replaySinceForLiveCatchup({ isLive = false, loadedSeq = 0, busy = false } = {}) {
  if (!isLive) return null;
  if (loadedSeq > 0) return loadedSeq;
  return busy ? 1 : null;
}

// 末尾 assistant 消息的文本(从尾部往前找第一条 assistant msg)。用于
// "内容回退" 探测与防回退保护:实时流式累积到完整内容后,任何把它替换成
// 更短文本的状态变更都是可疑的截断。
export function lastAssistantText(state) {
  const items = Array.isArray(state?.items) ? state.items : [];
  for (let i = items.length - 1; i >= 0; i -= 1) {
    const it = items[i];
    if (it?.kind === 'msg' && it.role === 'assistant') return String(it.content || '');
  }
  return '';
}

// 内容回退探测器(防回归诊断):某次状态变更把末尾 assistant 文本变短时报警,
// 标明来源路径、长度、前后预览,方便定位 "消息显示不全" 究竟由哪条路径造成。
// transcript_replace(重试/compact 的有意重置)会合法地变短,调用方应跳过。
// 低噪声:只在真的变短时打。
export function detectAssistantTailRegression(source, prevState, nextState) {
  const prev = lastAssistantText(prevState);
  const next = lastAssistantText(nextState);
  if (prev.length > next.length) {
    const tail = (s) => (s.length > 24 ? `${s.slice(0, 12)}…${s.slice(-12)}` : s);
    // eslint-disable-next-line no-console
    console.warn(
      `[ace-transcript] assistant tail shrank via ${source}: ${prev.length}→${next.length} chars`
      + ` lastSeq ${Number(prevState?.lastSeq) || 0}→${Number(nextState?.lastSeq) || 0}`
      + ` | was "${tail(prev)}" now "${tail(next)}"`,
    );
    return true;
  }
  return false;
}

// 防回退保护:REST 历史快照(loadedState)可能比实时 WS 已累积的当前回合更旧
// —— 快照里 messages 尚未含进行中的 assistant、events 只回放了部分 token。
// 当实时态(liveState)seq 不落后于快照、且末尾 assistant 文本更完整时,保留
// 实时文本,不让更旧的快照把界面截断。只增不减:返回值的末尾 assistant 文本
// 长度永远 >= 二者中较长的那个,因此即便误判也不会比直接用快照更差。
export function preserveLiveAssistantTailOnLoad(loadedState, liveState) {
  if (!loadedState) return loadedState;
  const liveItems = Array.isArray(liveState?.items) ? liveState.items : [];
  if (liveItems.length === 0) return loadedState;
  const liveSeq = Number(liveState?.lastSeq) || 0;
  const loadedSeq = Number(loadedState?.lastSeq) || 0;
  if (liveSeq <= loadedSeq) return loadedState; // 快照不比实时旧 → 用快照
  const liveTail = lastAssistantText(liveState);
  const loadedTail = lastAssistantText(loadedState);
  if (!liveTail || liveTail.length <= loadedTail.length) return loadedState;

  const items = loadedState.items.slice();
  let idx = -1;
  for (let i = items.length - 1; i >= 0; i -= 1) {
    if (items[i]?.kind === 'msg' && items[i].role === 'assistant') { idx = i; break; }
    if (items[i]?.kind === 'msg' || items[i]?.kind === 'tool') break;
  }
  if (idx >= 0) {
    items[idx] = { ...items[idx], content: liveTail };
  } else {
    const liveDraft = liveItems[liveItems.length - 1];
    const nextId = Number(loadedState.nextItemId) || 1;
    items.push({
      kind: 'msg',
      id: nextId,
      role: 'assistant',
      content: liveTail,
      contentParts: Array.isArray(liveDraft?.contentParts) ? liveDraft.contentParts : [],
      ts: liveDraft?.ts || Date.now(),
      streaming: liveState?.streamingId != null,
      streamDraft: true,
    });
    return {
      ...loadedState,
      items,
      nextItemId: nextId + 1,
      // 实时还在流式时,推入的草稿要接管 streamingId:后续 token 续在它后面,
      // 而不是另起一条草稿,把同一段正文拆成「前半截 + 完整版」两个气泡。
      streamingId: liveState?.streamingId != null ? nextId : (loadedState.streamingId ?? null),
      lastSeq: Math.max(loadedSeq, liveSeq),
    };
  }
  return {
    ...loadedState,
    items,
    lastSeq: Math.max(loadedSeq, liveSeq),
  };
}

// Initial history is fetched concurrently with the live WebSocket subscription.
// If a newer busy/progress frame arrives first, accepting an older REST runtime
// snapshot verbatim makes the composer and activity UI briefly fall back to
// idle. Preserve only the active foreground runtime fields here; transcript
// items still come from REST and the existing tail guard below.
export function preserveLiveRuntimeOnLoad(loadedState, liveState) {
  if (!loadedState) return loadedState;
  const liveSeq = Number(liveState?.lastSeq) || 0;
  const loadedSeq = Number(loadedState?.lastSeq) || 0;
  if (liveSeq <= loadedSeq) return loadedState;

  const liveIsRunning = isTranscriptActivelyRunning(liveState);
  if (!liveIsRunning) return loadedState;

  // Live work signals (reasoning/token/activity) can arrive before or without a
  // retained busy_changed(true). Never copy live.busy=false over an in-flight
  // turn — that is what drops Desktop 中止/排队 back to the idle send button.
  return {
    ...loadedState,
    busy: true,
    activeTurnId: String(liveState.activeTurnId || loadedState.activeTurnId || ''),
    status: 'running',
    activity: liveState.activity && typeof liveState.activity === 'object'
      ? { ...liveState.activity }
      : (loadedState.activity && typeof loadedState.activity === 'object'
        ? { ...loadedState.activity }
        : null),
    trajectoryPartial: liveState.trajectoryPartial && typeof liveState.trajectoryPartial === 'object'
      ? {
          ...liveState.trajectoryPartial,
          blocks: Array.isArray(liveState.trajectoryPartial.blocks)
            ? liveState.trajectoryPartial.blocks.map((block) => ({ ...block }))
            : [],
        }
      : (loadedState.trajectoryPartial && typeof loadedState.trajectoryPartial === 'object'
        ? {
            ...loadedState.trajectoryPartial,
            blocks: Array.isArray(loadedState.trajectoryPartial.blocks)
              ? loadedState.trajectoryPartial.blocks.map((block) => ({ ...block }))
              : [],
          }
        : null),
  };
}

export function applyTranscriptReplayEvents(state, events = []) {
  let next = cloneState(state || createTranscriptState());
  const effects = [];
  const seenMessageIds = new Set(next.items
    .filter((item) => item?.kind === 'msg' && item.messageId)
    .map((item) => item.messageId));
  const seenMessages = new Set(next.items
    .filter((item) => item?.kind === 'msg')
    .map((item) => messageKey(item.role || 'system', item.content || '')));
  let pendingStreamEvents = [];
  const flushPendingStreamEvents = () => {
    for (const ev of pendingStreamEvents) {
      const reduced = reduceDeferredStreamEvent(next, ev);
      next = reduced.state;
      effects.push(...reduced.effects);
    }
    pendingStreamEvents = [];
  };

  for (const ev of (Array.isArray(events) ? events : [])) {
    if (isStaleSequencedEvent(next, ev)) continue;
    if (ev?.type === 'token' || ev?.type === 'reasoning') {
      pendingStreamEvents.push(ev);
      continue;
    }
    if (ev?.type === 'message') {
      const p = ev.payload || {};
      const occurrenceIdentity = messageEventUsesOccurrenceIdentity(p);
      const incomingMessageId = p.id || '';
      if (!occurrenceIdentity && incomingMessageId && seenMessageIds.has(incomingMessageId)) {
        pendingStreamEvents = [];
        const reduced = reduceTranscriptEvent(next, ev);
        next = reduced.state;
        effects.push(...reduced.effects);
        continue;
      }
      const key = messageKey(p.role || 'system', p.content || '');
      if (!occurrenceIdentity && seenMessages.has(key)) {
        pendingStreamEvents = [];
        markEventSeqApplied(next, ev);
        continue;
      }
      if (!occurrenceIdentity) {
        if (incomingMessageId) seenMessageIds.add(incomingMessageId);
        seenMessages.add(key);
      }
    }
    if (eventFlushesPendingStream(ev)) flushPendingStreamEvents();
    const reduced = reduceTranscriptEvent(next, ev);
    next = reduced.state;
    effects.push(...reduced.effects);
  }
  flushPendingStreamEvents();

  return { state: next, effects };
}

export function createTranscriptState(overrides = {}) {
  return {
    items: [],
    busy: false,
    abortPending: false,
    activeTurnId: '',
    turns: 0,
    // 会话显示标题的三个服务端字段(与侧栏会话列表同源):title 是用户改名 /
    // 大模型生成的标题,summary 是无标题时的兜底(最近一条用户消息显示文本的
    // 80 字节截断)。对外暴露的 title 由 sessionDisplayTitle 从这三者派生。
    title: '',
    titleSource: '',
    summary: '',
    status: 'idle',
    lastSeq: 0,
    isLive: false,
    loadState: 'idle',
    historyBefore: '',
    historyAfter: '',
    historyHasMore: false,
    historyTurnTruncated: false,
    streamingId: null,
    trajectoryPartial: null,
    toolMap: new Map(),
    turnTimings: new Map(),
    turnNetDiffs: new Map(),
    nextItemId: 1,
    error: '',
    tokenUsage: null,
    goal: null,
    todos: [],
    todoSummary: null,
    activity: null,
    // turnHadAssistantText / lastAssistantText 用于桌面通知:在 busy=true→false
    // 转换且本回合产生过 assistant 文本时,emit turn_completed effect。reducer 之外
    // 的代码不应直接读 / 写它们。见
    // openspec/changes/add-windows-wintoast-completion-notifications。
    turnHadAssistantText: false,
    lastAssistantText: '',
    // 最近一个回合的收尾方式:'' (尚无 / 新回合已开始) | 'completed' | 'error' | 'aborted'。
    // 供 ChatView 的排队 drain effect 判断「这次 busy→false 是不是中断收尾」:
    // 中断收尾时排队消息要暂停而不是自动发出。本端点停止(turn_aborted)与
    // 远端中断(busy_changed / done 携带 outcome=aborted)都会置成 'aborted'。
    lastTurnOutcome: '',
    ...overrides,
    toolMap: cloneToolMap(overrides.toolMap),
    turnTimings: cloneTurnTimings(overrides.turnTimings),
    turnNetDiffs: cloneTurnNetDiffs(overrides.turnNetDiffs),
    tokenUsage: cloneTokenUsage(overrides.tokenUsage),
    goal: cloneGoal(overrides.goal),
    todos: cloneTodos(overrides.todos),
    todoSummary: overrides.todoSummary && typeof overrides.todoSummary === 'object' ? { ...overrides.todoSummary } : null,
  };
}

export function resetTranscriptForSession(state, { title = '', isLive = false } = {}) {
  return createTranscriptState({
    title,
    isLive,
    loadState: state?.loadState || 'idle',
  });
}

export function reduceTranscriptEvent(state, msg) {
  const current = state || createTranscriptState();
  if (isStaleSequencedEvent(current, msg)) {
    return { state: current, effects: [] };
  }

  const next = cloneState(current);
  const effects = [];
  const t = msg?.type || '';
  const p = msg?.payload || {};

  markEventSeqApplied(next, msg);

  switch (t) {
    case 'transcript_replace': {
      finalizeStreaming(next);
      next.trajectoryPartial = null;
      next.toolMap = new Map();
      const { messages, turnTimings, turnNetDiffs } = splitTranscriptMessages(p.messages);
      next.turnTimings = turnTimings;
      next.turnNetDiffs = turnNetDiffs;
      next.items = applyTurnMetadataToItems(
        historyItemsFromMessages(next, messages), turnTimings, turnNetDiffs);
      // 标题不从消息正文现推:title / summary 只认服务端(session_updated 与
      // messages 快照),否则顶部标题会变成最后一条 user 消息的全文,与侧栏不一致。
      next.tokenUsage = null;
      next.error = '';
      break;
    }
    case 'model_step_start': {
      const step = Number(p.step_index);
      next.trajectoryPartial = {
        turn: Math.max(1, (Number(next.turns) || 0) + 1),
        step: Number.isInteger(step) && step > 0 ? step : null,
        blocks: [],
      };
      markTranscriptRunning(next);
      break;
    }
    case 'agent_progress': {
      const phase = p.phase || '';
      const label = p.label || '';
      if (!phase && !label) break;
      markTranscriptRunning(next);
      next.activity = {
        phase,
        label: label || phase,
        detail: p.detail || '',
        // 工具前言(add-tool-preamble):当前阶段前言随进度帧透传,kind(read /
        // write)给以后的读放大镜 / 写笔触效果留位。
        preamble: preambleFromProgress(p),
        tool: p.tool || '',
        toolCallId: p.tool_call_id || p.call_id || p.id || '',
        toolIndex: p.tool_index ?? null,
        startedAtMs: Number(p.started_at_ms) || eventTs(msg),
        timestampMs: eventTs(msg),
        retryAttempt: Number.isFinite(Number(p.retry_attempt))
          ? Number(p.retry_attempt)
          : null,
        retryDelayMs: Number.isFinite(Number(p.retry_delay_ms))
          ? Number(p.retry_delay_ms)
          : null,
        retryAtMs: Number.isFinite(Number(p.retry_at_ms))
          ? Number(p.retry_at_ms)
          : null,
        retryMaxAttempts: Number.isFinite(Number(p.retry_max_attempts))
          ? Number(p.retry_max_attempts)
          : null,
      };
      break;
    }
    case 'queued_input_accepted': {
      const clientMessageId = typeof p.client_message_id === 'string'
        ? p.client_message_id.trim()
        : '';
      if (!clientMessageId) break;
      const alreadyVisible = next.items.some((item) => (
        item.kind === 'msg' &&
        item.role === 'user' &&
        clientMessageIdFromMetadata(item.metadata) === clientMessageId
      ));
      if (alreadyVisible) break;
      finalizeStreaming(next);
      next.items = [
        ...next.items,
        {
          kind: 'msg',
          id: allocateItemId(next),
          messageId: '',
          role: 'user',
          content: String(p.content || ''),
          contentParts: Array.isArray(p.content_parts) ? p.content_parts : [],
          ...composerContentFields(p),
          metadata: {
            client_message_id: clientMessageId,
            optimistic_queued_input: true,
          },
          ts: eventTs(msg),
        },
      ];
      break;
    }
    case 'message': {
      // 工具前言:assistant 正文里的 <text_preamble> 标签不进 transcript。
      const p = withVisibleAssistantContent(msg?.payload || {});
      const role = p.role || 'system';
      if (role === 'assistant' && p.metadata?.transcript_only === true &&
          p.metadata?.interrupted_output === true) {
        // A local stop can split one stream into drafts on both sides of its
        // optimistic notice. Replace that trailing span with the persisted
        // partial response; the confirmed stop notice follows it.
        let keep = next.items.length;
        while (keep > 0) {
          const item = next.items[keep - 1];
          const draft = item.kind === 'msg' && item.role === 'assistant' &&
            item.streamDraft && !item.messageId;
          const localStop = item.kind === 'termination_notice' && item.source === 'user' &&
            item.metadata?.user_aborted !== true;
          if (!draft && !localStop) break;
          keep -= 1;
        }
        if (keep < next.items.length) {
          next.items = next.items.slice(0, keep);
          next.streamingId = null;
        }
      }
      if (isUserAbortMessage(p)) {
        if (msg.replayed && p.id && next.items.some((item) => (
          item.kind === 'termination_notice' && item.messageId === p.id &&
          item.metadata?.retry_user_message_id === p.metadata.retry_user_message_id
        ))) break;
        finalizeStreaming(next);
        appendTerminationNotice(next, msg, { ...p, source: 'user' });
        break;
      }
      const turnNetDiff = normalizeTurnNetDiffRecord(p);
      if (turnNetDiff) {
        next.turnNetDiffs.set(turnNetDiff.userMessageUuid, turnNetDiff);
        next.items = applyTurnNetDiffsToItems(next.items, next.turnNetDiffs);
        break;
      }
      if (role === 'error') {
        // Legacy/current daemon provider failures are visible message events.
        // Clear any earlier assistant text so their terminal busy=false cannot
        // be mistaken for a completed turn.
        next.turnHadAssistantText = false;
        next.lastAssistantText = '';
      }
      const timing = normalizeTurnTimingRecord(p);
      if (timing) {
        next.turnTimings.set(timing.userMessageUuid, timing);
        next.items = applyTurnTimingsToItems(next.items, next.turnTimings);
        break;
      }
      if (role === 'assistant' && next.streamingId != null) {
        const currentStreamingId = next.streamingId;
        next.streamingId = null;
        const finalContent = p.content || '';
        if (finalContent && finalContent.trim()) {
          next.turnHadAssistantText = true;
          next.lastAssistantText = finalContent;
        }
        const isStreamingDraft = (item) => item.id === currentStreamingId;
        // WS 带旧游标订阅时,REST 已加载的 assistant 会被回放的 token 重新
        // 流成草稿,再由这条 replayed message 定稿 —— 那就是同一条消息的第二份。
        const duplicateIndex = replayedDuplicateMessageIndex(next.items, p, msg, isStreamingDraft);
        if (duplicateIndex >= 0) {
          next.items = dropDraftAndMergeReplayedMessage(
            next.items, duplicateIndex, isStreamingDraft, p, msg);
          break;
        }
        next.items = next.items.map((item) => (isStreamingDraft(item)
          ? replaceAssistantItemWithFinal(item, p, msg)
          : item));
        break;
      }
      if (role === 'assistant' && (p.content || '').trim()) {
        const draftIndex = trailingAssistantDraftIndex(next.items);
        if (draftIndex >= 0) {
          const isTrailingDraft = (item, index) => index === draftIndex;
          const duplicateIndex = replayedDuplicateMessageIndex(next.items, p, msg, isTrailingDraft);
          if (duplicateIndex >= 0) {
            next.items = dropDraftAndMergeReplayedMessage(
              next.items, duplicateIndex, isTrailingDraft, p, msg);
            break;
          }
          next.items = next.items.map((item, index) => (index === draftIndex
            ? replaceAssistantItemWithFinal(item, p, msg)
            : item));
          break;
        }
      }
      finalizeStreaming(next);
      const incomingContent = p.content || '';
      if (role === 'assistant' && incomingContent && incomingContent.trim()) {
        next.turnHadAssistantText = true;
        next.lastAssistantText = incomingContent;
      }
      // 按持久消息 id 幂等(dedupe-message-events-by-id):home auto_start 流程中
      // GET /messages 快照与 WS 回放存在竞争窗口 —— daemon 先读事件回放、后读
      // 消息快照,而回合线程先 append 消息(中间隔落盘 I/O)、后 emit 事件,
      // 交错时快照已含 user 消息但其 message 事件 seq 高于水位、随后才经 WS
      // 送达。seq 水位只保证通道内幂等,跨通道要靠消息 id:命中已有条目时
      // 原位更新(事件可能带更完整的 content_parts / metadata),不追加,
      // 否则用户气泡出现两次。不同 id 相同文本(用户故意连发)不受影响。
      const incomingMessageId = p.id || '';
      // provider/runtime error 不属于持久消息。不同回合可能得到完全相同的
      // role/content/id,必须按事件 seq 分别展示;同一事件重放仍由上方水位拦截。
      const existingIndex = incomingMessageId && !messageEventUsesOccurrenceIdentity(p)
        ? next.items.findIndex((item) => item.kind === 'msg' && item.messageId === incomingMessageId)
        : -1;
      if (existingIndex >= 0) {
        next.items = next.items.map((item, index) => (index === existingIndex
          ? mergeMessageEventIntoItem(item, p, msg)
          : item));
        break;
      }
      const incomingClientMessageId = role === 'user'
        ? clientMessageIdFromMetadata(p.metadata)
        : '';
      const optimisticIndex = incomingClientMessageId
        ? next.items.findIndex((item) => (
            item.kind === 'msg' &&
            item.role === 'user' &&
            !item.messageId &&
            item.metadata?.optimistic_queued_input === true &&
            clientMessageIdFromMetadata(item.metadata) === incomingClientMessageId
          ))
        : -1;
      if (optimisticIndex >= 0) {
        next.items = next.items.map((item, index) => (index === optimisticIndex
          ? {
              ...item,
              messageId: incomingMessageId,
              role,
              content: incomingContent,
              contentParts: Array.isArray(p.content_parts) ? p.content_parts : item.contentParts,
              metadata: p.metadata,
              ...composerContentFields(p, item),
              ts: eventTs(msg),
            }
          : item));
        break;
      }
      next.items = [
        ...next.items,
        {
          kind: 'msg',
          id: allocateItemId(next),
          messageId: incomingMessageId,
          role,
          content: incomingContent,
          contentParts: Array.isArray(p.content_parts) ? p.content_parts : [],
          metadata: p.metadata,
          ...composerContentFields(p),
          ts: eventTs(msg),
        },
      ];
      break;
    }
    case 'token': {
      const text = p.text || '';
      appendTrajectoryPartialText(next, 'text', text);
      if (text) markTranscriptRunning(next);
      if (text && text.trim()) {
        next.turnHadAssistantText = true;
      }
      if (next.streamingId == null) {
        if (!text.trim()) break;
        const id = allocateItemId(next);
        next.streamingId = id;
        next.items = [
          ...next.items,
          {
            kind: 'msg',
            id,
            role: 'assistant',
            content: text,
            ts: eventTs(msg),
            streaming: true,
            streamDraft: true,
          },
        ];
        next.lastAssistantText = text;
      } else {
        const currentStreamingId = next.streamingId;
        next.items = next.items.map((item) => {
          if (item.id !== currentStreamingId) return item;
          const merged = (item.content || '') + text;
          next.lastAssistantText = merged;
          return { ...item, content: merged, ts: eventTs(msg) };
        });
      }
      break;
    }
    case 'reasoning': {
      const text = p.text || '';
      appendTrajectoryPartialText(next, 'reasoning', text);
      if (text) markTranscriptRunning(next);
      break;
    }
    case 'tool_start': {
      markTranscriptRunning(next);
      finalizeStreaming(next);
      const persistedIndex = replayedPersistedToolIndex(
        next, p.tool_call_id || p.call_id || p.id || '', msg);
      if (persistedIndex >= 0) {
        next.toolMap.set(toolKey(p), next.items[persistedIndex].id);
        break;
      }
      const id = allocateItemId(next);
      next.toolMap.set(toolKey(p), id);
      const tool = {
        isTaskComplete: !!p.is_task_complete,
        isDone: false,
        success: null,
        tool: p.tool || '',
        toolCallId: p.tool_call_id || p.call_id || p.id || '',
        toolIndex: p.tool_index ?? null,
        args: cloneJsonLike(p.args),
        startedAtMs: eventTs(msg),
        displayOverride: p.display_override || '',
        title: p.display_override || p.command_preview || `${p.tool || ''}  ${JSON.stringify(p.args || {})}`,
        tailLines: [],
        currentPartial: '',
        totalLines: 0,
        totalBytes: 0,
        elapsed: 0,
        summary: p.is_task_complete ? { object: (p.args && p.args.summary) || '完成' } : null,
        output: '',
        hunks: [],
        attachments: [],
        metadata: null,
        askUserQuestionResult: null,
      };
      // 工具前言(add-tool-preamble):tool_start 直接带这次调用沿用的阶段前言,
      // 运行中的工具行与实时活动行用它当标题;落定后不再显示。
      const startPreamble = preambleFromToolStart(p);
      if (startPreamble) tool.preamble = startPreamble;
      next.items = [...next.items, { kind: 'tool', id, tool, ts: eventTs(msg) }];
      break;
    }
    case 'tool_update': {
      const id = next.toolMap.get(toolKey(p));
      if (!id) break;
      next.items = next.items.map((item) => {
        if (item.id !== id || item.kind !== 'tool') return item;
        return {
          ...item,
          ts: eventTs(msg),
          tool: {
            ...item.tool,
            tailLines: p.tail_lines || item.tool.tailLines,
            currentPartial: p.current_partial || '',
            totalLines: p.total_lines || item.tool.totalLines,
            totalBytes: p.total_bytes || item.tool.totalBytes,
            elapsed: p.elapsed_seconds || item.tool.elapsed,
            toolCallId: p.tool_call_id || item.tool.toolCallId || '',
            toolIndex: p.tool_index ?? item.tool.toolIndex ?? null,
          },
        };
      });
      break;
    }
    case 'tool_end': {
      const key = toolKey(p);
      const id = next.toolMap.get(key);
      next.toolMap.delete(key);
      if (!id) break;
      next.items = next.items.map((item) => {
        if (item.id !== id || item.kind !== 'tool') return item;
        return {
          ...item,
          messageId: p.message_id || item.messageId || '',
          ts: eventTs(msg),
          tool: {
            ...item.tool,
            isDone: true,
            success: !!p.success,
            summary: p.summary || item.tool.summary || fallbackToolSummary(
              item.tool.tool || p.tool,
              item.tool.args,
            ),
            output: p.output || '',
            hunks: Array.isArray(p.hunks) ? p.hunks : [],
            attachments: normalizeAttachmentList(p.attachments),
            metadata: p.metadata || item.tool.metadata || null,
            askUserQuestionResult: normalizeAskUserQuestionResult(p.metadata) || item.tool.askUserQuestionResult || null,
            elapsed: p.elapsed_seconds || item.tool.elapsed,
            toolCallId: p.tool_call_id || item.tool.toolCallId || '',
            toolIndex: p.tool_index ?? item.tool.toolIndex ?? null,
          },
        };
      });
      break;
    }
    case 'turn_diff': {
      const turnNetDiff = normalizeTurnNetDiffRecord(p);
      if (!turnNetDiff) break;
      next.turnNetDiffs.set(turnNetDiff.userMessageUuid, turnNetDiff);
      next.items = applyTurnNetDiffsToItems(next.items, next.turnNetDiffs);
      break;
    }
    case 'usage': {
      next.tokenUsage = normalizeUsagePayload(p, eventTs(msg));
      break;
    }
    case 'goal_updated': {
      next.goal = cloneGoal(p.goal);
      break;
    }
    case 'goal_cleared': {
      next.goal = null;
      break;
    }
    case 'todo_updated': {
      const todos = normalizeTodos(p.todos);
      next.todos = todos;
      next.todoSummary = normalizeTodoSummary(p.summary, todos);
      break;
    }
    case 'session_updated': {
      // 显示标题的唯一来源:title(用户改名 / 大模型生成)与 summary(无标题时
      // 的兜底,服务端已截到 80 字节)。侧栏 Sidebar 对同一事件做同样的合并。
      if (Object.prototype.hasOwnProperty.call(p, 'title')) {
        next.title = p.title || '';
        next.titleSource = String(p.title_source || '');
      }
      if (Object.prototype.hasOwnProperty.call(p, 'summary')) {
        next.summary = typeof p.summary === 'string' ? p.summary : '';
      }
      break;
    }
    case 'busy_changed': {
      const wasBusy = !!state?.busy;
      const outcome = typeof p.outcome === 'string' ? p.outcome : '';
      const completedOutcome = !outcome || outcome === 'completed';
      next.busy = !!p.busy;
      next.abortPending = false;
      next.activeTurnId = next.busy ? String(p.turn_id || '') : '';
      next.status = next.busy ? 'running' : 'idle';
      if (next.busy && !wasBusy) {
        // 回合开始 → 重置桌面通知用的回合标记
        next.turnHadAssistantText = false;
        next.lastAssistantText = '';
        next.trajectoryPartial = null;
        next.lastTurnOutcome = '';
      }
      if (!next.busy) {
        next.activity = null;
        next.trajectoryPartial = null;
        finalizeStreaming(next);
        if (wasBusy) next.turns = (next.turns || 0) + 1;
        // 只在真正的 busy→false 转换上记收尾方式;本端点停止后服务端补发的
        // busy_changed(false) 到达时 wasBusy 已是 false,不覆盖 turn_aborted 记下的 aborted。
        if (wasBusy) next.lastTurnOutcome = outcome || 'completed';
        if (wasBusy && completedOutcome && next.turnHadAssistantText) {
          effects.push({
            type: 'turn_completed',
            payload: { final_assistant_text: next.lastAssistantText || '' },
          });
        }
        // The daemon emits busy_changed(false) followed by done. Consume the
        // marker here so the trailing done frame cannot emit a duplicate toast.
        if (wasBusy) {
          next.turnHadAssistantText = false;
          next.lastAssistantText = '';
        }
      }
      break;
    }
    case 'done': {
      const wasBusy = !!state?.busy;
      const outcome = typeof p.outcome === 'string' ? p.outcome : '';
      const completedOutcome = !outcome || outcome === 'completed';
      next.busy = false;
      next.abortPending = false;
      next.activeTurnId = '';
      next.status = 'idle';
      next.activity = null;
      next.trajectoryPartial = null;
      finalizeStreaming(next);
      if (wasBusy) next.lastTurnOutcome = outcome || 'completed';
      if (wasBusy && completedOutcome && next.turnHadAssistantText) {
        effects.push({
          type: 'turn_completed',
          payload: { final_assistant_text: next.lastAssistantText || '' },
        });
      }
      next.turnHadAssistantText = false;
      next.lastAssistantText = '';
      break;
    }
    case 'error':
      next.busy = false;
      next.abortPending = false;
      next.activeTurnId = '';
      next.status = 'error';
      next.error = p.reason || '';
      next.activity = null;
      next.trajectoryPartial = null;
      finalizeStreaming(next);
      next.lastTurnOutcome = 'error';
      next.turnHadAssistantText = false;
      next.lastAssistantText = '';
      appendTerminationNotice(next, msg, { ...p, source: p.source || 'server' });
      effects.push({ type: 'error', payload: p });
      break;
    case 'turn_aborted':
      next.busy = false;
      next.abortPending = true;
      next.activeTurnId = '';
      next.status = 'idle';
      next.activity = null;
      next.trajectoryPartial = null;
      finalizeStreaming(next);
      next.lastTurnOutcome = 'aborted';
      next.turnHadAssistantText = false;
      next.lastAssistantText = '';
      appendTerminationNotice(next, msg, { ...p, source: 'user' });
      break;
    case 'permission_request':
      effects.push({ type: 'permission_request', payload: p });
      break;
    case 'question_request':
      effects.push({ type: 'question_request', payload: p });
      break;
    default:
      break;
  }

  return { state: next, effects };
}


export const HISTORY_PAGE_SIZE = 200;

// Prepending keeps every existing item ID (and therefore the DOM/window anchor).
// Runtime events remain owned by the live store while disk pages arrive.
export function mergeTranscriptHistoryPage(state, data = {}, direction = 'before') {
  const current = state || createTranscriptState();
  const messages = Array.isArray(data.messages) ? data.messages : [];
  const next = cloneState(current);
  const split = splitTranscriptMessages(messages);
  const added = historyItemsFromMessages(next, split.messages);
  for (const [key, value] of split.turnTimings) next.turnTimings.set(key, value);
  for (const [key, value] of split.turnNetDiffs) next.turnNetDiffs.set(key, value);
  next.items = messages.length === 0 ? current.items : applyTurnMetadataToItems(
    direction === 'before' ? [...added, ...current.items] : [...current.items, ...added],
    next.turnTimings, next.turnNetDiffs,
  );
  if (direction === 'before') {
    next.historyBefore = data.before || '';
    next.historyHasMore = data.has_more === true;
    next.historyTurnTruncated = data.turn_truncated === true;
  } else {
    next.historyAfter = data.after || current.historyAfter;
  }
  return next;
}

export async function fetchCompletedTurnHistory(api, sid, state) {
  const items = Array.isArray(state?.items) ? state.items : [];
  let anchor = null;
  let count = 0;
  for (let i = items.length - 1; i >= 0; i -= 1) {
    count += 1;
    if (items[i]?.kind === 'msg' && items[i].role === 'user') { anchor = items[i]; break; }
  }
  const limit = Math.min(1000, Math.max(HISTORY_PAGE_SIZE, count + 16));
  const tail = await api.getMessages(sid, { limit });
  const containsAnchor = (tail.messages || []).some((m) => (
    m.role === 'user' && (anchor?.messageId ? m.id === anchor.messageId : m.content === anchor?.content)
  ));
  if (!containsAnchor && tail.has_more && tail.before) {
    const older = await api.getMessages(sid, { limit, before: tail.before });
    return { ...tail, messages: [...(older.messages || []), ...(tail.messages || [])] };
  }
  return tail;
}

export function loadTranscriptHistory(state, data = {}) {
  const current = state || createTranscriptState();
  let next = createTranscriptState({
    title: current.title || '',
    titleSource: current.titleSource || '',
    summary: current.summary || '',
    status: current.status || 'idle',
    isLive: !!current.isLive,
    lastSeq: 0,
    loadState: 'loaded',
    historyBefore: data.before || '',
    historyAfter: data.after || '',
    historyHasMore: data.has_more === true,
    historyTurnTruncated: data.turn_truncated === true,
  });
  const effects = [];
  const { messages: msgs, turnTimings, turnNetDiffs } = splitTranscriptMessages(data.messages);

  next.turnTimings = turnTimings;
  next.turnNetDiffs = turnNetDiffs;
  next.items = applyTurnMetadataToItems(
    historyItemsFromMessages(next, msgs), turnTimings, turnNetDiffs);

  // messages 快照(GET /messages?since=0)随历史一并带回服务端的标题三件套;
  // 深链 / 刷新打开的会话没有侧栏对象可抄,顶部标题靠它对齐侧栏。
  if (typeof data.title === 'string') {
    next.title = data.title;
    next.titleSource = typeof data.title_source === 'string' ? data.title_source : '';
  }
  if (typeof data.summary === 'string') next.summary = data.summary;

  const seenMessages = new Set(msgs.map((m) => messageKey(m.role || 'system', m.content || '')));
  let pendingStreamEvents = [];
  const flushPendingStreamEvents = () => {
    for (const ev of pendingStreamEvents) {
      const reduced = reduceDeferredStreamEvent(next, ev);
      next = reduced.state;
      effects.push(...reduced.effects);
    }
    pendingStreamEvents = [];
  };

  for (const ev of (Array.isArray(data.events) ? data.events : [])) {
    if (isStaleSequencedEvent(next, ev)) continue;
    if (ev?.type === 'token' || ev?.type === 'reasoning') {
      pendingStreamEvents.push(ev);
      continue;
    }
    if (ev?.type === 'message') {
      const p = ev.payload || {};
      const occurrenceIdentity = messageEventUsesOccurrenceIdentity(p);
      const key = messageKey(p.role || 'system', p.content || '');
      if (!occurrenceIdentity && seenMessages.has(key)) {
        pendingStreamEvents = [];
        markEventSeqApplied(next, ev);
        continue;
      }
      if (!occurrenceIdentity) seenMessages.add(key);
    }
    if (eventFlushesPendingStream(ev)) flushPendingStreamEvents();
    const reduced = reduceTranscriptEvent(next, ev);
    next = reduced.state;
    effects.push(...reduced.effects);
  }
  flushPendingStreamEvents();

  if (Object.prototype.hasOwnProperty.call(data, 'goal')) {
    next.goal = cloneGoal(data.goal);
  }
  if (Object.prototype.hasOwnProperty.call(data, 'todos')) {
    const todos = normalizeTodos(data.todos);
    next.todos = todos;
    next.todoSummary = normalizeTodoSummary(data.todo_summary ?? data.todoSummary, todos);
  }
  const restoredTurnCount = readRuntimeTurnCount(data);
  if (restoredTurnCount !== null) {
    next.turns = restoredTurnCount;
  }
  const restoredUsage = data.token_usage ?? data.tokenUsage ?? data.latest_token_usage ?? data.latestTokenUsage;
  if (restoredUsage && typeof restoredUsage === 'object') {
    next.tokenUsage = normalizeUsagePayload(restoredUsage, Date.now());
  }
  if (data.busy === true) {
    next.busy = true;
    next.activeTurnId = String(data.active_turn_id || data.activeTurnId || '');
    next.status = 'running';
  } else if (data.busy === false && next.status !== 'error') {
    next.busy = false;
    next.activeTurnId = '';
    next.status = 'idle';
    next.activity = null;
    next.trajectoryPartial = null;
    finalizeStreaming(next);
  }

  return { state: next, effects };
}

export function canLiveMonitorSession(sessionRef, live = 'auto') {
  const ref = normalizeSessionRef(sessionRef);
  // Optimistic navigation can render a disk-backed session before the daemon
  // has finished registering its runtime entry. This safety boundary must win
  // even when the caller generally enables live monitoring for writable chats.
  if (ref?.resumePending === true || ref?.resumeFailed === true) return false;
  if (live === true) return true;
  if (live === false || !ref) return false;
  const status = ref.status || ref.attention_state || ref.read_state || '';
  return !!(
    ref.active ||
    ref.busy ||
    status === 'running' ||
    status === 'waiting' ||
    status === 'in_progress'
  );
}

export function projectCompactTranscriptItems(items, limit = 6) {
  const source = normalizeToolInvocationItems(items);
  const boundedLimit = Math.max(1, Number(limit) || 1);
  return source.slice(-boundedLimit);
}

function dispatchEffects(effects, sid, options) {
  if (!effects || effects.length === 0) return;
  for (const effect of effects) {
    const payload = { ...(effect.payload || {}) };
    if (sid && !payload.session_id) payload.session_id = sid;
    if (effect.type === 'error') options.onError?.(payload.reason || '');
    if (effect.type === 'permission_request') options.onPermissionRequest?.(payload);
    if (effect.type === 'question_request') options.onQuestionRequest?.(payload);
    if (effect.type === 'turn_completed') options.onTurnCompleted?.(payload);
  }
}

export function useSessionTranscript(sessionRef, options = {}) {
  const ref = useMemo(() => normalizeSessionRef(sessionRef), [sessionRef]);
  const sid = ref?.sessionId || '';
  const api = useMemo(() => createApi(ref), [ref?.port, ref?.token, ref?.workspaceHash]);
  const liveMode = options.live ?? 'auto';
  const isLive = !!sid && canLiveMonitorSession(ref, liveMode);
  const liveRef = useRef(isLive);
  liveRef.current = isLive;
  const resumeReplayRef = useRef({ sid, pending: !isLive });
  if (resumeReplayRef.current.sid !== sid) resumeReplayRef.current = { sid, pending: !isLive };
  if (!isLive) resumeReplayRef.current.pending = true;
  const refreshIntervalMs = Math.max(
    0,
    Number(options.refreshIntervalMs) || 0,
  );
  // 实时状态归 store 所有,React 只订阅。历史上这里是 useState + 一个可变
  // 引用的双写:被动 effect 把渲染快照写回引用,吞掉这之后已到达的 token,
  // 表现为长会话流式出字时正文中间随机缺字(见 singleWriterStore.js 顶部注释)。
  const storeRef = useRef(null);
  if (storeRef.current === null) {
    storeRef.current = createSingleWriterStore(
      createTranscriptState({
        ...sessionTitleFieldsFromRef(ref),
        isLive,
        loadState: sid ? 'loading' : 'idle',
      }),
    );
  }
  const store = storeRef.current;
  const subscribeStore = useCallback((listener) => store.subscribe(listener), [store]);
  const getStoreSnapshot = useCallback(() => store.getState(), [store]);
  const state = useSyncExternalStore(subscribeStore, getStoreSnapshot, getStoreSnapshot);
  const optionsRef = useRef(options);
  // 加载 effect 里只用 ref 取展示标题,不依赖它的对象身份 —— 见下方 deps 注释。
  const sessionRefRef = useRef(ref);
  // state 在 sid 切换后的首帧仍属于上一会话;在 reset effect 落地前不能把
  // 旧会话的 loaded 状态当成新会话已加载。
  const stateSessionIdRef = useRef(sid);
  const historyScopeRef = useRef(null);
  const loadingHistoryRef = useRef(false);
  const pendingHistoryEventsRef = useRef([]);
  sessionRefRef.current = ref;
  const refreshSignatureRef = useRef('');

  useEffect(() => { optionsRef.current = options; }, [options]);

  const applyEvent = useCallback((msg, { emitEffects = true } = {}) => {
    if (loadingHistoryRef.current) pendingHistoryEventsRef.current.push(msg);
    let effects = [];
    // reducer 的附带产物用闭包带出 producer:先把数据落进 store,再派发副作用。
    const nextState = store.commit((prevState) => {
      const reduced = reduceTranscriptEvent(prevState, msg);
      effects = reduced.effects;
      // transcript_replace 是重试/compact 的有意重置,合法变短,不报警。
      if (msg?.type !== 'transcript_replace') {
        detectAssistantTailRegression(`event:${msg?.type || '?'}`, prevState, reduced.state);
      }
      return reduced.state;
    });
    if (emitEffects) dispatchEffects(effects, sid, optionsRef.current);
    return nextState;
  }, [sid, store]);


  const pageRequestRef = useRef(null);
  const reloadHistoryTail = useCallback(async () => {
    const scope = historyScopeRef.current;
    const data = await api.getMessages(sid, { limit: HISTORY_PAGE_SIZE });
    if (historyScopeRef.current !== scope) return null;
    store.commit((previous) => preserveLiveAssistantTailOnLoad(
      preserveLiveRuntimeOnLoad(loadTranscriptHistory(previous, data).state, previous), previous,
    ));
    return data;
  }, [api, sid, store]);

  const loadEarlier = useCallback(async (target = null) => {
    if (pageRequestRef.current) return pageRequestRef.current;
    const scope = historyScopeRef.current;
    const previous = store.getState();
    if (previous.loadState !== 'loaded') return null;
    const position = target?.messagePosition;
    if (position != null && previous.items.some((item) => item.messagePosition === String(position))) return String(position);
    if (!target && !previous.historyHasMore) return null;
    const query = { limit: HISTORY_PAGE_SIZE };
    if (previous.historyBefore && target?.messageOrdinal == null) query.before = previous.historyBefore;
    if (target?.messagePosition != null) query.from_position = String(target.messagePosition);
    else if (target?.messageOrdinal != null) query.from_ordinal = target.messageOrdinal;
    const pending = (async () => {
      try {
        const data = await api.getMessages(sid, query);
        if (historyScopeRef.current !== scope) return null;
        // A legacy ordinal may resolve inside the already loaded tail. In that
        // case its response is used only to resolve the position, without overlap.
        store.commit((current) => {
          if (current.historyBefore !== previous.historyBefore) return current;
          const existing = new Set(current.items.map((item) => item.messagePosition).filter((p) => p != null));
          if (target && existing.has(String(data.messages?.[0]?.message_position))) return current;
          const rows = (data.messages || []).filter((m) => !existing.has(String(m.message_position)));
          return mergeTranscriptHistoryPage(current, { ...data, messages: rows }, 'before');
        });
        return data.messages?.[0]?.message_position ?? null;
      } catch (error) {
        if (historyScopeRef.current !== scope) return null;
        if (error?.status === 409) { await reloadHistoryTail(); return null; }
        optionsRef.current.onError?.('加载会话失败:' + (error?.message || ''));
        return null;
      }
    })();
    pageRequestRef.current = pending;
    try { return await pending; }
    finally { if (pageRequestRef.current === pending) pageRequestRef.current = null; }
  }, [api, sid, store, reloadHistoryTail]);

  const getState = useCallback(() => store.getState(), [store]);

  // 只接受 producer:值形式允许调用方传入一份过期快照,正是本次修复要根除的
  // 写法。调用方需要“读当前状态 -> 计算 -> 写回”时,整段计算放进 producer。
  const updateState = useCallback((producer) => store.commit(producer), [store]);

  useEffect(() => {
    store.commit((previous) => previous.isLive === isLive
      ? previous
      : { ...previous, isLive });
  }, [isLive, store]);

  useEffect(() => {
    const isLive = liveRef.current;
    stateSessionIdRef.current = sid;
    const baseTitleFields = sessionTitleFieldsFromRef(sessionRefRef.current);
    const sameHistory = historyScopeRef.current?.sid === sid
      && historyScopeRef.current?.api === api;
    historyScopeRef.current = { sid, api };
    loadingHistoryRef.current = !!sid;
    pendingHistoryEventsRef.current = [];
    pageRequestRef.current = null;
    if (sameHistory) {
      // Runtime recovery changes live eligibility, not transcript identity.
      // Keep the disk history visible while fetching the live catch-up.
      store.commit((previous) => ({ ...previous, isLive }));
    } else {
      store.commit(() => createTranscriptState({
        ...baseTitleFields,
        isLive,
        loadState: sid ? 'loading' : 'idle',
      }));
      refreshSignatureRef.current = '';
    }
    if (!sid) return undefined;
    ensureSessionOpen(sid);

    let off = false;
    api.getMessages(sid, { limit: HISTORY_PAGE_SIZE }).then((data) => {
      if (off) return;
      loadingHistoryRef.current = false;
      const pendingEvents = pendingHistoryEventsRef.current;
      pendingHistoryEventsRef.current = [];
      const isLive = liveRef.current;
      const messages = Array.isArray(data?.messages) ? data.messages : [];
      refreshSignatureRef.current = `${messages.length}:${
        messages.length > 0 ? JSON.stringify(messages[messages.length - 1]) : ''
      }`;
      let loadEffects = [];
      // 读取当前状态、与实时尾巴合并、写回必须是一次原子提交:拆成多步读写会
      // 重新打开“基于过期快照计算”的窗口。
      const nextState = store.commit((prevState) => {
        const loaded = loadTranscriptHistory(prevState, data || {});
        loadEffects = loaded.effects;
        // A recovery audit/live event can arrive after the disk read's fixed
        // boundary but before its response. Reapply it without emitting effects twice.
        loaded.state = applyTranscriptReplayEvents(loaded.state, pendingEvents).state;
        // 防回退:实时 WS 可能在 getMessages(0) 解析期间已累积了更完整的当前
        // 回合内容,而这份 REST 快照更旧(messages 尚未含进行中的 assistant)。
        // 直接覆盖会把界面截断,这里保留更完整的实时尾巴。
        const runtimeGuarded = preserveLiveRuntimeOnLoad(loaded.state, prevState);
        const guarded = preserveLiveAssistantTailOnLoad(runtimeGuarded, prevState);
        const merged = {
          ...guarded,
          isLive,
          loadState: 'loaded',
        };
        detectAssistantTailRegression('load', prevState, merged);
        return merged;
      });
      dispatchEffects(loadEffects, sid, optionsRef.current);

      const loadedSeq = nextState.lastSeq || 0;
      // busy 取防回退保护之后的值:REST 快照说 idle 但实时 WS 已看到更新的
      // busy 帧时,preserveLiveRuntimeOnLoad 会把 busy 提回 true —— 此时仍
      // 需要补拉重建进行中的草稿。
      const replaySince = replaySinceForLiveCatchup({
        isLive,
        loadedSeq,
        busy: !!nextState.busy,
      });
      if (replaySince !== null) {
        api.getMessages(sid, replaySince).then((replayData) => {
          if (off) return;
          const replayEvents = Array.isArray(replayData)
            ? replayData
            : (Array.isArray(replayData?.events) ? replayData.events : []);
          if (replayEvents.length > 0) {
            let replayEffects = [];
            store.commit((prevState) => {
              const replayed = applyTranscriptReplayEvents(prevState, replayEvents);
              replayEffects = replayed.effects;
              detectAssistantTailRegression('catchup', prevState, replayed.state);
              return replayed.state;
            });
            dispatchEffects(replayEffects, sid, optionsRef.current);
            return;
          }
          if (loadedSeq === 0 && Array.isArray(replayData?.messages)) {
            let refreshEffects = [];
            store.commit((prevState) => {
              const refreshed = loadTranscriptHistory(prevState, replayData);
              refreshEffects = refreshed.effects;
              const refreshedState = {
                ...refreshed.state,
                isLive,
                loadState: 'loaded',
              };
              detectAssistantTailRegression('refresh', prevState, refreshedState);
              return refreshedState;
            });
            dispatchEffects(refreshEffects, sid, optionsRef.current);
          }
        }).catch(() => {});
      }
    }).catch((error) => {
      if (off) return;
      loadingHistoryRef.current = false;
      pendingHistoryEventsRef.current = [];
      store.commit((prevState) => ({
        ...prevState,
        loadState: 'error',
        error: error?.message || 'load failed',
      }));
      optionsRef.current.onError?.('加载会话失败:' + (error?.message || ''));
    });

    return () => { off = true; };
    // deps 刻意不含 ref(对象身份):App 侧 replaceActiveRef / 元数据更新会在
    // sid 不变的情况下造出新的 ref 对象,如果 ref 在 deps 里,每次都会把
    // transcript 整个重置回 loading 并重拉全量历史(feedback IQSZ-D0668 的
    // desktop 日志里 "tail shrank via catchup: lastSeq 0→NNNN" 就是这条路径
    // 的可见后果)。真正需要重载的输入只有:会话身份(sid)、连接目标(api,
    // 已按 port/token/workspaceHash memo)。实时性单独更新,不会取消仍在途的
    // 磁盘加载;成为实时会话时由下方 WebSocket 订阅回放补齐事件。
  }, [api, sid]);

  useEffect(() => {
    if (!sid || stateSessionIdRef.current !== sid || state.loadState !== 'loaded') return undefined;
    let secondFrame = null;
    const firstFrame = requestAnimationFrame(() => {
      secondFrame = requestAnimationFrame(() => {
        void reportSessionOpen(sid, (payload) => api.reportSessionOpen(payload));
      });
    });
    return () => {
      cancelAnimationFrame(firstFrame);
      if (secondFrame !== null) cancelAnimationFrame(secondFrame);
    };
  }, [api, sid, state.loadState]);

  useEffect(() => {
    if (!sid || refreshIntervalMs < 250) return undefined;
    let stopped = false;
    let inFlight = false;
    const refresh = () => {
      if (stopped || inFlight) return;
      inFlight = true;
      const after = store.getState().historyAfter;
      if (!after) { inFlight = false; return; }
      api.getMessages(sid, { after })
        .then((data) => {
          if (stopped) return;
          store.commit((current) => current.historyAfter !== after ? current
            : mergeTranscriptHistoryPage(current, data, 'after'));
        })
        .catch(async (error) => {
          if (!stopped && error?.status === 409) await reloadHistoryTail();
        })
        .finally(() => {
          inFlight = false;
        });
    };
    const timer = window.setInterval(refresh, refreshIntervalMs);
    return () => {
      stopped = true;
      window.clearInterval(timer);
    };
  }, [api, isLive, refreshIntervalMs, sid, reloadHistoryTail, store]);

  useEffect(() => {
    if (!sid || !isLive) return undefined;
    connection.reconfigure({ port: ref?.port || '', token: ref?.token || '' });
    const handler = (event) => {
      const msg = event.detail || {};
      const msgSid = msg.session_id || msg.payload?.session_id || '';
      if (msgSid && msgSid !== sid) return;
      applyEvent(msg);
    };
    connection.addEventListener('message', handler);
    connection.retainSession(sid, resumeReplayRef.current.pending
      ? { since: store.getState().lastSeq || 0, replayFromStart: true }
      : {});
    resumeReplayRef.current.pending = false;
    return () => {
      connection.removeEventListener('message', handler);
      connection.releaseSession(sid);
    };
  }, [applyEvent, isLive, ref?.port, ref?.token, sid]);

  const activelyRunning = isTranscriptActivelyRunning(state);
  return {
    ...state,
    // 顶部标题栏显示的就是它:与侧栏同一份 sessionDisplayTitle 派生规则,
    // 只是 title / summary 取 store 里被 session_updated 实时刷新过的值。
    title: transcriptDisplayTitle(state, sessionRefRef.current),
    isLive,
    busy: activelyRunning,
    status: activelyRunning ? 'running' : state.status,
    loadState: stateSessionIdRef.current === sid
      ? state.loadState
      : (sid ? 'loading' : 'idle'),
    applyEvent,
    loadEarlier,
    getState,
    updateState,
  };
}
