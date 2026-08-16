import { deriveTrajectoryLayout } from '../components/trajectory/deepseek/layout.ts';

const MODEL_EVENT_TYPES = new Set([
  'model_step_start',
  'model_request',
  'model_first_output',
  'model_response',
  'model_step_finish',
]);

const TOOL_EVENT_TYPES = new Set(['tool_start', 'tool_end']);

function finiteNumber(value) {
  if (value === null || value === undefined) return null;
  if (typeof value === 'string' && value.trim() === '') return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function timestampOf(record, ...payloadKeys) {
  const payload = record?.payload || {};
  for (const key of payloadKeys) {
    const value = finiteNumber(payload[key]);
    if (value !== null) return value;
  }
  return finiteNumber(record?.timestamp_ms);
}

function safeJson(value, pretty = false) {
  if (value == null) return '';
  if (typeof value === 'string') return value;
  try {
    return JSON.stringify(value, null, pretty ? 2 : 0);
  } catch {
    return String(value);
  }
}

function compactText(value, maxLength = 180) {
  const text = safeJson(value).replace(/\s+/g, ' ').trim();
  return text.length > maxLength ? `${text.slice(0, maxLength - 1)}…` : text;
}

function recordOrder(record) {
  const sequence = finiteNumber(record?.sequence);
  if (sequence !== null) return [1, sequence];
  return [0, finiteNumber(record?.legacy_index) ?? 0];
}

export function trajectoryRecordKey(record) {
  const sequence = finiteNumber(record?.sequence);
  if (sequence !== null) return `recorded:${sequence}`;
  const legacyIndex = finiteNumber(record?.legacy_index);
  if (legacyIndex !== null) return `legacy:${legacyIndex}`;
  return `unknown:${record?.type || 'event'}:${safeJson(record?.payload)}`;
}

export function mergeTrajectoryRecords(previous = [], incoming = []) {
  const byKey = new Map();
  for (const record of [...previous, ...incoming]) {
    if (!record || typeof record !== 'object') continue;
    byKey.set(trajectoryRecordKey(record), record);
  }
  return [...byKey.values()].sort((left, right) => {
    const a = recordOrder(left);
    const b = recordOrder(right);
    return a[0] - b[0] || a[1] - b[1];
  });
}

function makeTurn(number, id) {
  return {
    number,
    id,
    inputs: [],
    steps: new Map(),
    activeStep: 0,
  };
}

function makeStep(number, order) {
  return {
    number,
    order,
    start: null,
    request: null,
    firstOutput: null,
    response: null,
    finish: null,
    tools: [],
    toolByKey: new Map(),
    retries: [],
  };
}

function toolLifecycleKey(record) {
  const payload = record?.payload || {};
  if (payload.tool_call_id) return `call:${payload.tool_call_id}`;
  if (payload.id) return `call:${payload.id}`;
  const index = payload.tool_index ?? record?.legacy_index ?? record?.sequence ?? '';
  return `${payload.tool || payload.name || 'tool'}:${index}`;
}

function ensureStep(turn, stepNumber, order) {
  let number = Number(stepNumber);
  if (!Number.isInteger(number) || number <= 0) {
    number = turn.activeStep > 0 ? turn.activeStep : turn.steps.size + 1;
  }
  let step = turn.steps.get(number);
  if (!step) {
    step = makeStep(number, order);
    turn.steps.set(number, step);
  }
  turn.activeStep = number;
  return step;
}

function sourceBlock(value) {
  if (value == null || typeof value !== 'object') {
    return { type: 'text', content: safeJson(value) };
  }
  const type = typeof value.type === 'string' ? value.type : 'unknown';
  const text = typeof value.text === 'string'
    ? value.text
    : (typeof value.content === 'string' ? value.content : '');
  if (text) return { type: type === 'reasoning' ? 'thinking' : type, content: text };
  const url = typeof value.url === 'string'
    ? value.url
    : (typeof value.image_url === 'string' ? value.image_url : '');
  if (url && /^(?:https?:|blob:|data:image\/)/.test(url)) {
    return { type, content: '', imageSrc: url, imageAlt: value.alt || undefined };
  }
  return { type, content: safeJson(value, true) };
}

function contentBlocks(payload) {
  if (Array.isArray(payload?.content_parts) && payload.content_parts.length > 0) {
    return payload.content_parts.map(sourceBlock);
  }
  if (Array.isArray(payload?.content)) return payload.content.map(sourceBlock);
  const text = typeof payload?.content === 'string' ? payload.content : '';
  return text ? [{ type: 'text', content: text }] : [];
}

function contentText(payload) {
  if (typeof payload?.content === 'string') return payload.content;
  return contentBlocks(payload)
    .filter((block) => block.type === 'text' || block.type === 'input_text')
    .map((block) => block.content)
    .join('\n');
}

function messageCellBase(record, kind) {
  const payload = record?.payload || {};
  const content = contentText(payload) || safeJson(payload.message || payload.text || '');
  return {
    recordId: trajectoryRecordKey(record),
    kind,
    text: '',
    ...(content ? { previewMarkdown: content, inputDetail: content } : {}),
    ...(contentBlocks(payload).length > 0 ? { sourceBlocks: contentBlocks(payload) } : {}),
    ...(finiteNumber(record?.sequence) === null
      ? {}
      : { sourceSeq: finiteNumber(record.sequence) }),
    timeSeconds: 0,
    startedAt: timestampOf(record),
  };
}

function systemPrompt(request) {
  if (!request) return null;
  const messages = Array.isArray(request.messages) ? request.messages : [];
  const system = messages
    .filter((message) => message?.role === 'system' || message?.role === 'developer')
    .map((message) => contentText(message) || safeJson(message?.content))
    .filter(Boolean)
    .join('\n\n');
  const tools = (Array.isArray(request.tools) ? request.tools : []).map((tool) => ({
    name: String(tool?.native_name || tool?.name || ''),
    description: String(tool?.description || ''),
    parameters: tool?.parameters ?? {},
  }));
  return { system, tools };
}

function samePrompt(left, right) {
  return safeJson(left) === safeJson(right);
}

function promptChangeLabel(previous, current) {
  if (!previous) return 'Initial System Prompt';
  const systemChanged = previous.system !== current.system;
  const toolsChanged = safeJson(previous.tools) !== safeJson(current.tools);
  if (systemChanged && toolsChanged) return 'System Prompt and Tools Updated';
  if (systemChanged) return 'System Prompt Updated';
  return 'Tools Updated';
}

function usageFromPayload(value) {
  if (!value || typeof value !== 'object') return undefined;
  const prompt = finiteNumber(value.prompt_tokens ?? value.input_tokens);
  const cacheRead = finiteNumber(value.cache_read_tokens);
  const cacheWrite = finiteNumber(value.cache_write_tokens);
  const completion = finiteNumber(value.completion_tokens ?? value.output_tokens);
  const reasoning = finiteNumber(value.reasoning_tokens);
  const hasAny = [prompt, cacheRead, cacheWrite, completion, reasoning]
    .some((item) => item !== null);
  if (!hasAny) return undefined;
  const uncachedInput = prompt === null
    ? null
    : Math.max(0, prompt - (cacheRead ?? 0));
  return {
    ...(uncachedInput === null ? {} : { input: uncachedInput }),
    ...(cacheRead === null ? {} : { cacheRead }),
    ...(cacheWrite === null ? {} : { cacheWrite }),
    ...(completion === null ? {} : { output: completion }),
    ...(reasoning === null ? {} : { reasoning }),
  };
}

function addUsage(total, usage) {
  if (!usage) return total;
  const next = {};
  for (const key of ['input', 'cacheRead', 'cacheWrite', 'output', 'reasoning']) {
    if (total?.[key] !== undefined || usage[key] !== undefined) {
      next[key] = (total?.[key] || 0) + (usage[key] || 0);
    }
  }
  return next;
}

function requestConfig(request) {
  if (!request) return undefined;
  const config = {
    provider: request.provider,
    model: request.model,
    context_window: request.context_window,
    context_usage_estimate: request.context_usage_estimate,
    prompt_diagnostics: request.prompt_diagnostics,
  };
  return Object.fromEntries(Object.entries(config).filter(([, value]) => value !== undefined));
}

function requestStatus(step) {
  const value = String(step.response?.payload?.status || step.finish?.payload?.reason || '');
  if (value === 'error' || value === 'aborted' || value === 'failed') return 'error';
  if (!step.response && !step.finish) return 'running';
  return 'complete';
}

function assistantCell(step, turnNumber) {
  const requestRecord = step.request;
  const responseRecord = step.response;
  const request = requestRecord?.payload || {};
  const response = responseRecord?.payload || {};
  const finish = step.finish?.payload || {};
  const content = contentText(response);
  const thinking = typeof response.reasoning_content === 'string'
    ? response.reasoning_content
    : '';
  const calls = Array.isArray(response.tool_calls) ? response.tool_calls : [];
  const sourceBlocks = [
    ...(thinking ? [{ type: 'thinking', content: thinking }] : []),
    ...(content ? [{ type: 'text', content }] : contentBlocks(response)),
    ...calls.map((call) => ({
      type: 'tool-call',
      content: safeJson(call?.arguments ?? call?.args ?? ''),
      callId: call?.id || undefined,
      toolName: call?.name || call?.function?.name || undefined,
    })),
  ];
  const start = timestampOf(step.start || requestRecord);
  const first = timestampOf(step.firstOutput);
  const completed = timestampOf(step.finish || responseRecord);
  const usage = usageFromPayload(response.usage || finish.usage);
  const outputTokens = usage?.output;
  const sourceSeq = finiteNumber(responseRecord?.sequence ?? requestRecord?.sequence);
  return {
    recordId: `assistant\u0000${turnNumber}\u0000${step.number}`,
    kind: 'message',
    text: content || thinking ? '' : (calls.length > 0 ? 'Tool call only' : ''),
    ...(content ? { previewMarkdown: content, outputDetail: content } : {}),
    ...(!content && thinking ? { previewMarkdown: thinking } : {}),
    ...(thinking ? { thinkingDetail: thinking } : {}),
    ...(sourceBlocks.length > 0 ? { sourceBlocks } : {}),
    ...(sourceSeq === null ? {} : { sourceSeq }),
    timeSeconds: start === null || completed === null
      ? null
      : Math.max(0, (completed - start) / 1000),
    startedAt: start,
    ...(usage?.input === undefined ? {} : { input: usage.input }),
    ...(usage?.cacheRead === undefined ? {} : { cacheRead: usage.cacheRead }),
    ...(usage?.cacheWrite === undefined ? {} : { cacheWrite: usage.cacheWrite }),
    ...(outputTokens === undefined ? {} : { output: outputTokens }),
    ...(usage?.reasoning === undefined ? {} : { think: usage.reasoning }),
    ...(requestStatus(step) === 'error' ? { isError: true } : {}),
    assistantMetrics: {
      timingRecorded: start !== null || first !== null || completed !== null,
      stepStartTime: start,
      firstTokenTime: first,
      completedTime: completed,
      usageProvided: usage !== undefined,
      outputTokens: outputTokens ?? null,
    },
    _request: request,
    _response: response,
  };
}

function toolCell(lifecycle, schema) {
  const startRecord = lifecycle.start;
  const endRecord = lifecycle.end;
  const payload = { ...(startRecord?.payload || {}), ...(endRecord?.payload || {}) };
  const name = String(payload.tool || payload.name || 'tool');
  const args = startRecord?.payload?.args ?? payload.arguments;
  const output = endRecord?.payload?.output
    ?? endRecord?.payload?.content
    ?? (lifecycle.legacy ? lifecycle.legacy.payload?.content : undefined);
  const callId = payload.tool_call_id || payload.id || undefined;
  const start = timestampOf(startRecord || lifecycle.legacy, 'started_at_ms');
  const completed = timestampOf(endRecord || lifecycle.legacy, 'completed_at_ms');
  const duration = finiteNumber(endRecord?.payload?.duration_ms);
  const success = endRecord?.payload?.success;
  const sourceSeq = finiteNumber(endRecord?.sequence ?? startRecord?.sequence);
  const outputText = safeJson(output, typeof output !== 'string');
  return {
    recordId: callId ? `tool\u0000call\u0000${callId}` : trajectoryRecordKey(endRecord || startRecord || lifecycle.legacy),
    kind: 'tool',
    text: name,
    ...(args === undefined ? {} : {
      previewMarkdown: safeJson(args),
      inputDetail: safeJson(args, typeof args !== 'string'),
    }),
    ...(outputText ? {
      outputDetail: outputText,
      outputBlocks: [{ type: 'text', content: outputText }],
    } : {}),
    ...(success === false
      ? { result: endRecord?.payload?.failure_stage || 'error' }
      : outputText ? { result: '', resultPreviewMarkdown: outputText } : { result: 'No output' }),
    ...(schema ? { schemaDetail: safeJson(schema, true) } : {}),
    ...(callId ? { callId } : {}),
    ...(success === false ? { isError: true } : {}),
    ...(sourceSeq === null ? {} : { sourceSeq }),
    timeSeconds: duration !== null
      ? Math.max(0, duration / 1000)
      : start === null || completed === null
        ? null
        : Math.max(0, (completed - start) / 1000),
    startedAt: start,
  };
}

function toolSchema(step, name) {
  const tools = step.request?.payload?.tools;
  if (!Array.isArray(tools)) return null;
  return tools.find((tool) => tool?.native_name === name || tool?.name === name) || null;
}

function formatGroupDuration(milliseconds) {
  if (!Number.isFinite(milliseconds)) return '';
  return `${Math.max(0, Math.round(milliseconds)).toLocaleString('en-US')} ms`;
}

function describeGroup(cells) {
  const times = [];
  const tools = new Map();
  for (const cell of cells) {
    if (Number.isFinite(cell.startedAt)) {
      times.push(cell.startedAt);
      if (Number.isFinite(cell.timeSeconds)) times.push(cell.startedAt + cell.timeSeconds * 1000);
    }
    if (cell.kind === 'tool') tools.set(cell.text, (tools.get(cell.text) || 0) + 1);
  }
  const parts = [];
  if (times.length >= 2) parts.push(formatGroupDuration(Math.max(...times) - Math.min(...times)));
  for (const [name, count] of tools) parts.push(count > 1 ? `${name}×${count}` : name);
  return parts.filter(Boolean).join(' ') || undefined;
}

function normalizeInputRecord(record) {
  const payload = record?.payload || {};
  if (record.type === 'legacy_context') {
    if (payload?.metadata?.transcript_only === true || payload.content === '[Turn net diff]') {
      return null;
    }
    return messageCellBase(record, 'context');
  }
  if (record.type === 'legacy_user_message') {
    return { ...messageCellBase(record, 'user'), opensTurn: true };
  }
  if (record.type !== 'message') return null;
  if (payload.role === 'user') {
    return {
      ...messageCellBase(record, payload.is_meta ? 'context' : 'user'),
      ...(payload.is_meta ? {} : { opensTurn: true }),
    };
  }
  if (payload.role === 'system' || payload.role === 'context') {
    return messageCellBase(record, 'context');
  }
  if (payload.role === 'error') {
    return { ...messageCellBase(record, 'context'), text: 'Error' };
  }
  return null;
}

function runtimeSequence(record, fallback) {
  const sequence = finiteNumber(record?.sequence);
  if (sequence !== null) return sequence;
  const legacyIndex = finiteNumber(record?.legacy_index);
  return -1_000_000_000 + (legacyIndex ?? fallback);
}

function runtimeTime(record, ...payloadKeys) {
  return timestampOf(record, ...payloadKeys) ?? Number.NaN;
}

function runtimeContentBlock(value) {
  if (typeof value === 'string') return { type: 'text', text: value };
  if (value == null || typeof value !== 'object') {
    return { type: 'text', text: safeJson(value) };
  }
  const type = String(value.type || 'unknown');
  const normalizedType = type === 'thinking' ? 'reasoning' : type;
  const text = typeof value.text === 'string'
    ? value.text
    : (typeof value.content === 'string' ? value.content : null);
  if (text !== null) return { ...value, type: normalizedType, text };
  return { ...value, type: normalizedType };
}

function runtimeContentBlocks(payload, fallbackValue) {
  if (Array.isArray(payload?.content_parts) && payload.content_parts.length > 0) {
    return payload.content_parts.map(runtimeContentBlock);
  }
  if (Array.isArray(payload?.content) && payload.content.length > 0) {
    return payload.content.map(runtimeContentBlock);
  }
  const value = fallbackValue ?? payload?.content ?? payload?.message ?? payload?.text;
  if (value === undefined || value === null || value === '') return [];
  return [runtimeContentBlock(value)];
}

function runtimeInputNode(record, fallback) {
  const payload = record?.payload || {};
  let kind = null;
  if (record.type === 'legacy_user_message') kind = 'user';
  else if (record.type === 'legacy_context') kind = 'context';
  else if (record.type === 'message') {
    if (payload.role === 'user' && !payload.is_meta) kind = 'user';
    else if (['user', 'system', 'context', 'error'].includes(payload.role)) kind = 'context';
  }
  if (kind === null) return null;
  return {
    kind,
    seq: runtimeSequence(record, fallback),
    time: runtimeTime(record),
    content: runtimeContentBlocks(payload),
    source: payload.source ?? payload.metadata ?? null,
    ...(kind === 'context' ? { provenance: null, form: null } : {}),
  };
}

function runtimeUsage(value) {
  const usage = usageFromPayload(value);
  if (!usage) return undefined;
  return {
    ...(usage.input === undefined ? {} : { inputTokens: usage.input }),
    ...(usage.cacheRead === undefined ? {} : { cacheReadTokens: usage.cacheRead }),
    ...(usage.cacheWrite === undefined ? {} : { cacheWriteTokens: usage.cacheWrite }),
    ...(usage.output === undefined ? {} : { outputTokens: usage.output }),
    ...(usage.reasoning === undefined ? {} : { reasoningTokens: usage.reasoning }),
  };
}

function runtimeRequestConfig(request) {
  if (!request) return undefined;
  const config = {
    provider: request.provider,
    model: request.model,
    context_window: request.context_window,
    context_usage_estimate: request.context_usage_estimate,
    prompt_diagnostics: request.prompt_diagnostics,
  };
  const entries = Object.entries(config).filter(([, value]) => value !== undefined);
  return entries.length === 0 ? undefined : Object.fromEntries(entries);
}

function runtimePrompt(request) {
  if (!request) return undefined;
  const prompt = systemPrompt(request);
  if (!prompt) return undefined;
  return {
    config: runtimeRequestConfig(request) || {},
    system: prompt.system,
    tools: prompt.tools,
  };
}

function promptChange(previous, current, seq, time) {
  if (!current) return undefined;
  if (!previous) return { seq, time, kind: 'initial' };
  const systemChanged = previous.system !== current.system;
  const toolsChanged = safeJson(previous.tools) !== safeJson(current.tools);
  if (!systemChanged && !toolsChanged) return undefined;
  return {
    seq,
    time,
    kind: systemChanged && toolsChanged
      ? 'system-and-tools'
      : (systemChanged ? 'system' : 'tools'),
    previous,
  };
}

function normalizedRequestStatus(step) {
  const responseStatus = String(step.response?.payload?.status || '');
  const finishReason = String(step.finish?.payload?.reason || '');
  const failed = ['error', 'failed', 'aborted', 'cancelled', 'retry', 'empty_response_retry'];
  if (failed.includes(responseStatus) || failed.includes(finishReason)) return 'error';
  if (!step.response && !step.finish) return 'running';
  return 'complete';
}

function runtimeAssistantNode(step, turnNumber, fallback) {
  const responseRecord = step.response;
  if (!responseRecord) return null;
  const response = responseRecord.payload || {};
  const blocks = [];
  if (typeof response.reasoning_content === 'string' && response.reasoning_content !== '') {
    blocks.push({ kind: 'reasoning', text: response.reasoning_content });
  }
  const contentParts = runtimeContentBlocks(response);
  let hasText = false;
  for (const block of contentParts) {
    if (block.type === 'text' || block.type === 'input_text' || block.type === 'output_text') {
      const text = typeof block.text === 'string' ? block.text : '';
      if (text !== '') {
        blocks.push({ kind: 'text', text });
        hasText = true;
      }
    } else if (block.type === 'reasoning' || block.type === 'thinking') {
      const text = typeof block.text === 'string' ? block.text : '';
      if (text !== '' && text !== response.reasoning_content) {
        blocks.push({ kind: 'reasoning', text });
      }
    } else if (String(block.type).toLowerCase().includes('image')) {
      blocks.push({ kind: 'image', attachment: block.attachment ?? block });
    } else {
      blocks.push({ kind: 'other', block });
    }
  }
  if (!hasText && typeof response.content === 'string' && response.content !== '') {
    blocks.push({ kind: 'text', text: response.content });
  }
  for (const call of Array.isArray(response.tool_calls) ? response.tool_calls : []) {
    blocks.push({
      kind: 'tool-call',
      callId: String(call?.id || ''),
      name: String(call?.name || call?.function?.name || 'tool'),
      argsRaw: safeJson(call?.arguments ?? call?.args ?? ''),
    });
  }
  const request = step.request?.payload || {};
  const startedAt = timestampOf(step.start || step.request);
  const firstTokenTime = timestampOf(step.firstOutput);
  const completedAt = timestampOf(step.finish || responseRecord);
  const usage = runtimeUsage(response.usage || step.finish?.payload?.usage);
  const status = normalizedRequestStatus(step);
  const interruptionEvidence = blocks.some((block) => {
    if (block.kind === 'text' || block.kind === 'reasoning') return block.text.trim() !== '';
    return true;
  });
  if (status === 'error' && !interruptionEvidence) return null;
  if (status === 'error') {
    const boundary = step.finish || responseRecord;
    return {
      kind: 'assistant',
      seq: runtimeSequence(boundary, fallback) - 0.9,
      time: runtimeTime(boundary),
      turn: turnNumber,
      step: step.number,
      blocks,
      interrupted: true,
    };
  }
  return {
    kind: 'assistant',
    seq: runtimeSequence(responseRecord, fallback),
    ...(response.message_id ? { messageId: response.message_id } : {}),
    time: completedAt ?? runtimeTime(responseRecord),
    turn: turnNumber,
    step: step.number,
    blocks,
    ...(usage ? { usage } : {}),
    ...((response.provider || request.provider) && (response.model || request.model)
      ? {
        provenance: {
          provider: response.provider || request.provider,
          model: response.model || request.model,
        },
      }
      : {}),
    ...(runtimeRequestConfig(request) ? { requestConfig: runtimeRequestConfig(request) } : {}),
    timing: {
      stepStartTime: startedAt,
      firstTokenTime,
      completedTime: completedAt ?? runtimeTime(responseRecord),
    },
  };
}

function runtimeToolResultNode(lifecycle, fallback) {
  const startRecord = lifecycle.start;
  const endRecord = lifecycle.end || lifecycle.legacy;
  if (!endRecord) return null;
  const startPayload = startRecord?.payload || {};
  const endPayload = endRecord.payload || {};
  const payload = { ...startPayload, ...endPayload };
  const callId = String(payload.tool_call_id || payload.id || '');
  const name = String(payload.tool || payload.name || 'tool');
  const argsRaw = safeJson(startPayload.args ?? payload.arguments ?? '');
  const output = endPayload.output ?? endPayload.content ?? endPayload.result ?? '';
  const content = Array.isArray(output)
    ? output.map(runtimeContentBlock)
    : [{ type: 'text', text: safeJson(output, typeof output !== 'string') }];
  return {
    kind: 'tool-result',
    seq: runtimeSequence(endRecord, fallback),
    time: runtimeTime(endRecord, 'completed_at_ms'),
    callId,
    call: { name, argsRaw },
    callTime: timestampOf(startRecord || endRecord, 'started_at_ms'),
    content,
    isError: endPayload.success === false || endPayload.ok === false,
    ...(endPayload.error && typeof endPayload.error === 'object'
      ? { error: endPayload.error }
      : {}),
    ...(endPayload.metadata === undefined ? {} : { meta: endPayload.metadata }),
    callView: null,
    resultView: null,
    subCalls: [],
  };
}

function requestNumberUsage(value) {
  return usageFromPayload(value);
}

function buildRequestNumbers(nodes, requests) {
  const assistantsByStep = new Map();
  for (const node of nodes) {
    if (node.kind !== 'assistant' || node.step <= 0) continue;
    assistantsByStep.set(`${node.turn}\u0000${node.step}`, node);
  }
  const requestsByStep = new Map(
    requests
      .filter((request) => request.purpose === 'assistant')
      .map((request) => [`${request.turn}\u0000${request.step}`, request]),
  );
  const ordered = [
    ...requests.map((request) => ({
      seq: request.startSeq,
      request,
      node: request.purpose === 'assistant'
        ? assistantsByStep.get(`${request.turn}\u0000${request.step}`)
        : undefined,
    })),
    ...[...assistantsByStep.entries()].flatMap(([key, node]) => (
      requestsByStep.has(key) ? [] : [{ seq: node.seq, request: undefined, node }]
    )),
  ].sort((left, right) => left.seq - right.seq);
  const numbered = [];
  let cumulativeUsage;
  for (const [index, entry] of ordered.entries()) {
    const runtime = entry.request?.usage ?? entry.node?.usage;
    const usage = runtime === undefined ? undefined : {
      ...(runtime.inputTokens === undefined ? {} : { input: runtime.inputTokens }),
      ...(runtime.cacheReadTokens === undefined ? {} : { cacheRead: runtime.cacheReadTokens }),
      ...(runtime.cacheWriteTokens === undefined ? {} : { cacheWrite: runtime.cacheWriteTokens }),
      ...(runtime.outputTokens === undefined ? {} : { output: runtime.outputTokens }),
      ...(runtime.reasoningTokens === undefined ? {} : { reasoning: runtime.reasoningTokens }),
    };
    cumulativeUsage = addUsage(cumulativeUsage, usage);
    const request = entry.request;
    const node = entry.node;
    if (request?.purpose === 'compaction') {
      numbered.push({
        seq: request.startSeq,
        turn: request.turn,
        step: 0,
        group: `Compaction ${request.startSeq}`,
        number: index + 1,
        purpose: 'compaction',
        status: request.status,
        startedAt: request.startedAt,
        completedAt: request.completedAt,
        ...(request.error === undefined ? {} : { error: request.error }),
        resultSeq: request.resultSeq ?? request.startSeq,
        ...(request.provenance?.provider ? { provider: request.provenance.provider } : {}),
        ...(request.provenance?.model ? { model: request.provenance.model } : {}),
        ...(request.requestConfig ? { requestConfig: request.requestConfig } : {}),
        ...(usage ? { usage } : {}),
        ...(cumulativeUsage ? { cumulativeUsage } : {}),
      });
      continue;
    }
    const turn = request?.turn ?? node?.turn;
    const step = request?.step ?? node?.step;
    if (turn === undefined || step === undefined) continue;
    numbered.push({
      seq: entry.seq,
      turn,
      step,
      group: `Step ${step}`,
      number: index + 1,
      ...(request?.status === undefined ? {} : { status: request.status }),
      ...(request?.startedAt === undefined ? {} : { startedAt: request.startedAt }),
      ...(request?.completedAt === undefined ? {} : { completedAt: request.completedAt }),
      ...(request?.error === undefined ? {} : { error: request.error }),
      ...(request?.resultSeq === undefined ? {} : { resultSeq: request.resultSeq }),
      ...(request?.retry === undefined ? {} : { retry: request.retry }),
      ...(request?.maxRetries === undefined ? {} : { maxRetries: request.maxRetries }),
      ...(request?.retryDelayMs === undefined ? {} : { retryDelayMs: request.retryDelayMs }),
      ...(request?.provenance?.provider ?? node?.provenance?.provider
        ? { provider: request?.provenance?.provider ?? node?.provenance?.provider }
        : {}),
      ...(request?.provenance?.model ?? node?.provenance?.model
        ? { model: request?.provenance?.model ?? node?.provenance?.model }
        : {}),
      ...(request?.requestConfig ?? node?.requestConfig
        ? { requestConfig: request?.requestConfig ?? node?.requestConfig }
        : {}),
      ...(usage ? { usage } : {}),
      ...(cumulativeUsage ? { cumulativeUsage } : {}),
    });
  }
  return numbered;
}

/**
 * Translate ACECode's append-only trajectory records into the exact grouped
 * ledger contract consumed by DeepSeek Harness's trajectory components.
 */
export function buildDeepSeekTrajectory(records = []) {
  const ordered = mergeTrajectoryRecords([], records);
  const turns = [];
  const turnById = new Map();
  const compactions = new Map();
  let current = null;
  let nextTurn = 1;
  let order = 0;

  const openTurn = (id, forceNew = false) => {
    const key = id ? String(id) : '';
    if (!forceNew && key && turnById.has(key)) {
      current = turnById.get(key);
      return current;
    }
    if (!forceNew && current && (!key || current.id === key)) return current;
    const turn = makeTurn(nextTurn++, key || `turn-${nextTurn}`);
    turns.push(turn);
    if (key) turnById.set(key, turn);
    current = turn;
    return turn;
  };

  for (const record of ordered) {
    order += 1;
    const type = String(record?.type || '');
    const payload = record?.payload || {};
    const compactMetadata = payload?.metadata;
    if (type === 'message'
        && compactMetadata?.compact_notice === true
        && compactMetadata?.compact_notice_id) {
      const id = String(compactMetadata.compact_notice_id);
      let compaction = compactions.get(id);
      if (!compaction) {
        compaction = {
          id,
          startRecord: record,
          completedRecord: null,
          turn: current?.number ?? null,
          status: 'running',
          summary: undefined,
          rawOutput: undefined,
          error: undefined,
          retries: [],
        };
        compactions.set(id, compaction);
      }
      const stage = String(compactMetadata.compact_notice_stage || '');
      if (stage === 'progress') compaction.startRecord = record;
      if (stage === 'summary') {
        const text = String(payload.content || '').replace(/^\[Conversation summary\]\s*/u, '');
        compaction.summary = runtimeContentBlocks({}, text);
        compaction.rawOutput = compaction.summary;
        compaction.completedRecord = record;
        compaction.status = 'complete';
      }
      if (stage === 'error') {
        compaction.completedRecord = record;
        compaction.status = 'error';
        compaction.error = String(payload.content || 'Compaction failed');
      }
      if (compactMetadata.compact_notice_complete === true && compaction.completedRecord === null) {
        compaction.completedRecord = record;
        if (compaction.status === 'running') compaction.status = 'complete';
      }
      continue;
    }
    if (type === 'turn_start') {
      const turn = openTurn(payload.turn_id || payload.user_message_id, true);
      for (const alias of [payload.turn_id, payload.user_message_id]) {
        if (alias) turnById.set(String(alias), turn);
      }
      continue;
    }
    if (type === 'turn_end' || type === 'legacy_turn_end') {
      openTurn(payload.turn_id || payload.user_message_uuid);
      current = null;
      continue;
    }
    if (type === 'legacy_user_message') {
      openTurn(payload.uuid || payload.id || `legacy-${record.legacy_index}`, true)
        .inputs.push({ order, record });
      continue;
    }
    if (type === 'message' && payload.role === 'user' && !payload.is_meta) {
      const id = payload.id || payload.uuid || payload.message_id;
      const turn = openTurn(id);
      if (!turn.inputs.some((entry) => entry.record?.payload?.role === 'user')) {
        turn.inputs.push({ order, record });
      } else if (turn.id !== id) {
        openTurn(id, true).inputs.push({ order, record });
      } else {
        turn.inputs.push({ order, record });
      }
      continue;
    }
    const input = normalizeInputRecord(record);
    if (input) {
      (current || openTurn('')).inputs.push({ order, record });
      continue;
    }
    if (type === 'legacy_context'
        || (type === 'message' && ['system', 'context', 'error'].includes(payload.role))) {
      continue;
    }
    if (type === 'agent_progress' && payload.phase === 'model_retry') {
      const turn = current || openTurn('');
      const step = ensureStep(turn, payload.step_index, order);
      step.retries.push(record);
      continue;
    }
    if (MODEL_EVENT_TYPES.has(type)) {
      const turn = current || openTurn('');
      const step = ensureStep(turn, payload.step_index, order);
      if (type === 'model_step_start') step.start = record;
      if (type === 'model_request') step.request = record;
      if (type === 'model_first_output') step.firstOutput = record;
      if (type === 'model_response') step.response = record;
      if (type === 'model_step_finish') step.finish = record;
      continue;
    }
    if (TOOL_EVENT_TYPES.has(type)) {
      const turn = current || openTurn('');
      const step = ensureStep(turn, payload.step_index, order);
      const key = toolLifecycleKey(record);
      let lifecycle = step.toolByKey.get(key);
      if (!lifecycle) {
        lifecycle = { order, start: null, end: null, legacy: null };
        step.toolByKey.set(key, lifecycle);
        step.tools.push(lifecycle);
      }
      if (type === 'tool_start') lifecycle.start = record;
      else lifecycle.end = record;
      continue;
    }
    if (type === 'legacy_model_response') {
      const turn = current || openTurn('');
      const step = ensureStep(turn, turn.steps.size + 1, order);
      step.response = record;
      continue;
    }
    if (type === 'legacy_tool_result') {
      const turn = current || openTurn('');
      const step = ensureStep(turn, turn.activeStep || 1, order);
      const lifecycle = { order, start: null, end: null, legacy: record };
      step.tools.push(lifecycle);
      continue;
    }
  }

  const nodes = [];
  const requests = [];
  const runningCalls = [];
  const callSchemas = new Map();
  let previousPrompt;
  let fallback = 0;

  for (const turn of turns) {
    for (const { record } of turn.inputs.sort((left, right) => left.order - right.order)) {
      const node = runtimeInputNode(record, ++fallback);
      if (node) nodes.push(node);
    }

    const steps = [...turn.steps.values()].sort((left, right) => left.order - right.order);
    for (const step of steps) {
      const requestRecord = step.request;
      const requestPayload = requestRecord?.payload || {};
      const prompt = requestRecord ? runtimePrompt(requestPayload) : undefined;
      const requestSeq = runtimeSequence(
        requestRecord || step.start || step.response || step.finish,
        ++fallback,
      );
      const requestTime = runtimeTime(requestRecord || step.start || step.response);
      const change = prompt === undefined
        ? undefined
        : promptChange(previousPrompt, prompt, requestSeq, requestTime);
      if (prompt !== undefined) previousPrompt = prompt;

      const assistant = runtimeAssistantNode(step, turn.number, ++fallback);
      if (assistant) nodes.push(assistant);

      const responseCalls = new Map(
        (Array.isArray(step.response?.payload?.tool_calls)
          ? step.response.payload.tool_calls
          : [])
          .map((call) => [String(call?.id || ''), call]),
      );
      const requestTools = Array.isArray(requestPayload.tools) ? requestPayload.tools : [];

      for (const lifecycle of step.tools.sort((left, right) => left.order - right.order)) {
        const result = runtimeToolResultNode(lifecycle, ++fallback);
        const startPayload = lifecycle.start?.payload || {};
        const endPayload = lifecycle.end?.payload || lifecycle.legacy?.payload || {};
        const callId = String(
          result?.callId
            || startPayload.tool_call_id
            || endPayload.tool_call_id
            || endPayload.id
            || `legacy-call-${fallback}`,
        );
        const responseCall = responseCalls.get(callId);
        const name = String(
          startPayload.tool
            || endPayload.tool
            || responseCall?.name
            || responseCall?.function?.name
            || 'tool',
        );
        const argsRaw = safeJson(
          startPayload.args
            ?? endPayload.arguments
            ?? responseCall?.arguments
            ?? responseCall?.args
            ?? '',
        );
        const schema = requestTools.find((tool) => (
          tool?.native_name === name || tool?.name === name
        ));
        if (schema) {
          callSchemas.set(callId, {
            name: String(schema.native_name || schema.name || name),
            description: String(schema.description || ''),
            parameters: schema.parameters ?? {},
          });
        }
        if (result) {
          nodes.push({
            ...result,
            callId,
            call: { name, argsRaw },
          });
        } else {
          runningCalls.push({
            callId,
            name,
            argsRaw,
            turn: turn.number,
            step: step.number,
            time: runtimeTime(lifecycle.start, 'started_at_ms'),
            callView: null,
            subCalls: [],
          });
        }
      }

      if (requestRecord) {
        const status = normalizedRequestStatus(step);
        const startedAt = timestampOf(step.start || requestRecord);
        const completedAt = status === 'running'
          ? null
          : (assistant?.time ?? timestampOf(step.finish || step.response));
        const responsePayload = step.response?.payload || {};
        const provider = requestPayload.provider || responsePayload.provider;
        const model = requestPayload.model || responsePayload.model;
        const retry = step.retries.at(-1)?.payload;
        const errorValue = responsePayload.error ?? step.finish?.payload?.error
          ?? (status === 'error' ? step.finish?.payload?.reason : undefined);
        const usage = runtimeUsage(responsePayload.usage || step.finish?.payload?.usage);
        requests.push({
          purpose: 'assistant',
          startSeq: requestSeq,
          startedAt: startedAt ?? requestTime,
          completedAt,
          status,
          turn: turn.number,
          step: step.number,
          ...(prompt === undefined ? {} : { prompt }),
          ...(change === undefined ? {} : { promptChange: change }),
          ...(provider && model ? { provenance: { provider, model } } : {}),
          ...(runtimeRequestConfig(requestPayload)
            ? { requestConfig: runtimeRequestConfig(requestPayload) }
            : {}),
          ...(usage === undefined ? {} : { usage }),
          ...(assistant === null || assistant.interrupted === true
            ? {}
            : { resultSeq: assistant.seq }),
          ...(errorValue === undefined ? {} : { error: safeJson(errorValue) }),
          ...(retry?.retry_attempt === undefined ? {} : { retry: retry.retry_attempt }),
          ...(retry?.retry_max_attempts === undefined
            ? {}
            : { maxRetries: retry.retry_max_attempts }),
          ...(retry?.retry_delay_ms === undefined
            ? {}
            : { retryDelayMs: retry.retry_delay_ms }),
        });
      }
    }
  }

  for (const compaction of compactions.values()) {
    const startSeq = runtimeSequence(compaction.startRecord, ++fallback);
    const completedAt = compaction.completedRecord === null
      ? null
      : runtimeTime(compaction.completedRecord);
    requests.push({
      purpose: 'compaction',
      startSeq,
      startedAt: runtimeTime(compaction.startRecord),
      completedAt,
      status: compaction.status,
      turn: compaction.turn,
      step: 0,
      ...(compaction.error === undefined ? {} : { error: compaction.error }),
      ...(compaction.summary === undefined ? {} : { summary: compaction.summary }),
      ...(compaction.rawOutput === undefined ? {} : { rawOutput: compaction.rawOutput }),
      ...(compaction.completedRecord === null
        ? {}
        : { resultSeq: runtimeSequence(compaction.completedRecord, ++fallback) }),
    });
  }

  nodes.sort((left, right) => left.seq - right.seq);
  requests.sort((left, right) => left.startSeq - right.startSeq);
  const projectedTurns = deriveTrajectoryLayout({
    nodes,
    partial: null,
    runningCalls,
    requests,
    callSchemas,
  });
  const firstRecorded = ordered.find((record) => finiteNumber(record?.sequence) !== null);
  return {
    turns: projectedTurns,
    requestNumbers: buildRequestNumbers(nodes, requests),
    historyStartSeq: finiteNumber(firstRecorded?.sequence) ?? undefined,
    nodes,
    runningCalls,
    requests,
    callSchemas,
    latestTurnNumber: turns.at(-1)?.number,
  };
}

/** Attach ACECode's transient token/reasoning accumulator to its durable request. */
export function resolveDeepSeekTrajectoryPartial(projection, livePartial) {
  if (!projection || !livePartial) return null;
  const partialStep = finiteNumber(livePartial.step);
  const partialTurn = finiteNumber(livePartial.turn);
  const requests = Array.isArray(projection.requests) ? projection.requests : [];
  const partialRequest = [...requests].reverse().find((request) => (
    request.purpose === 'assistant'
      && request.status === 'running'
      && (partialTurn === null || request.turn === partialTurn)
      && (partialStep === null || request.step === partialStep)
  ));
  if (partialRequest === undefined) {
    if (partialStep === null) return null;
    return {
      turn: partialTurn
        ?? finiteNumber(projection.latestTurnNumber)
        ?? 1,
      step: partialStep,
      blocks: Array.isArray(livePartial.blocks)
        ? livePartial.blocks.map((block) => ({ ...block }))
        : [],
    };
  }
  const nodes = Array.isArray(projection.nodes) ? projection.nodes : [];
  if (nodes.some((node) => (
    node.kind === 'assistant'
      && node.turn === partialRequest.turn
      && node.step === partialRequest.step
  ))) return null;
  return {
    turn: partialRequest.turn,
    step: partialRequest.step,
    blocks: Array.isArray(livePartial.blocks)
      ? livePartial.blocks.map((block) => ({ ...block }))
      : [],
  };
}
