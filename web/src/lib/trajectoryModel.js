const MODEL_EVENT_TYPES = new Set([
  'model_step_start',
  'model_request',
  'model_first_output',
  'model_response',
  'model_step_finish',
]);

const TOOL_EVENT_TYPES = new Set(['tool_start', 'tool_end']);

function finiteNumber(value) {
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

/**
 * Translate ACECode's append-only trajectory records into the exact grouped
 * ledger contract consumed by DeepSeek Harness's trajectory components.
 */
export function buildDeepSeekTrajectory(records = []) {
  const ordered = mergeTrajectoryRecords([], records);
  const turns = [];
  const turnById = new Map();
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
    if (type === 'turn_start') {
      openTurn(payload.turn_id || payload.user_message_id, true);
      continue;
    }
    if (type === 'turn_end' || type === 'legacy_turn_end') {
      openTurn(payload.turn_id || payload.user_message_uuid);
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
    const turn = current || openTurn('');
    if (MODEL_EVENT_TYPES.has(type)) {
      const step = ensureStep(turn, payload.step_index, order);
      if (type === 'model_step_start') step.start = record;
      if (type === 'model_request') step.request = record;
      if (type === 'model_first_output') step.firstOutput = record;
      if (type === 'model_response') step.response = record;
      if (type === 'model_step_finish') step.finish = record;
      continue;
    }
    if (TOOL_EVENT_TYPES.has(type)) {
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
      const step = ensureStep(turn, turn.steps.size + 1, order);
      step.response = record;
      continue;
    }
    if (type === 'legacy_tool_result') {
      const step = ensureStep(turn, turn.activeStep || 1, order);
      const lifecycle = { order, start: null, end: null, legacy: record };
      step.tools.push(lifecycle);
      continue;
    }
  }

  let cellIndex = 0;
  let previousPrompt = null;
  let requestNumber = 0;
  let cumulativeUsage;
  const requestNumbers = [];
  const projectedTurns = [];

  for (const turn of turns) {
    const groups = [];
    const inputCells = turn.inputs
      .sort((left, right) => left.order - right.order)
      .map(({ record }) => normalizeInputRecord(record))
      .filter(Boolean)
      .map((cell) => ({ ...cell, index: ++cellIndex }));
    if (inputCells.length > 0) {
      groups.push({ title: 'Message', cells: inputCells, description: describeGroup(inputCells) });
    }

    const steps = [...turn.steps.values()].sort((left, right) => left.order - right.order);
    for (const step of steps) {
      const prompt = systemPrompt(step.request?.payload);
      if (prompt && !samePrompt(previousPrompt, prompt)) {
        const promptCell = {
          index: ++cellIndex,
          recordId: `system\u0000${turn.number}\u0000${step.number}`,
          kind: 'system',
          text: promptChangeLabel(previousPrompt, prompt),
          promptDetail: prompt,
          ...(previousPrompt ? { previousPromptDetail: previousPrompt } : {}),
          ...(finiteNumber(step.request?.sequence) === null
            ? {}
            : { sourceSeq: finiteNumber(step.request.sequence) }),
          timeSeconds: 0,
          startedAt: timestampOf(step.request),
        };
        if (!previousPrompt && groups[0]?.title === 'Message') {
          groups[0] = {
            ...groups[0],
            cells: [promptCell, ...groups[0].cells],
          };
        } else {
          groups.push({ title: 'Message', cells: [promptCell] });
        }
        previousPrompt = prompt;
      }

      const assistant = assistantCell(step, turn.number);
      assistant.index = ++cellIndex;
      const toolCells = step.tools
        .sort((left, right) => left.order - right.order)
        .map((lifecycle) => {
          const name = String(
            lifecycle.start?.payload?.tool
              || lifecycle.end?.payload?.tool
              || lifecycle.legacy?.payload?.tool
              || lifecycle.legacy?.payload?.name
              || 'tool',
          );
          return toolCell(lifecycle, toolSchema(step, name));
        })
        .map((cell) => ({ ...cell, index: ++cellIndex }));
      const cells = [assistant, ...toolCells];
      const group = `Step ${step.number}`;
      groups.push({ title: group, description: describeGroup(cells), cells });

      requestNumber += 1;
      const usage = usageFromPayload(
        step.response?.payload?.usage || step.finish?.payload?.usage,
      );
      cumulativeUsage = addUsage(cumulativeUsage, usage);
      const startedAt = timestampOf(step.start || step.request);
      const completedAt = timestampOf(step.finish || step.response);
      requestNumbers.push({
        ...(finiteNumber(step.request?.sequence) === null
          ? {}
          : { seq: finiteNumber(step.request.sequence) }),
        turn: turn.number,
        step: step.number,
        group,
        number: requestNumber,
        status: requestStatus(step),
        ...(startedAt === null ? {} : { startedAt }),
        ...(completedAt === null ? {} : { completedAt }),
        ...(finiteNumber(step.response?.sequence) === null
          ? {}
          : { resultSeq: finiteNumber(step.response.sequence) }),
        ...(step.request?.payload?.provider ? { provider: step.request.payload.provider } : {}),
        ...(step.request?.payload?.model ? { model: step.request.payload.model } : {}),
        ...(requestConfig(step.request?.payload)
          ? { requestConfig: requestConfig(step.request.payload) }
          : {}),
        ...(usage ? { usage } : {}),
        ...(cumulativeUsage ? { cumulativeUsage } : {}),
        ...(requestStatus(step) === 'error'
          ? { error: safeJson(step.response?.payload?.error || step.finish?.payload?.reason || '') }
          : {}),
      });
    }

    if (groups.length > 0) projectedTurns.push({ turn: turn.number, groups });
  }

  const firstRecorded = ordered.find((record) => finiteNumber(record?.sequence) !== null);
  return {
    turns: projectedTurns,
    requestNumbers,
    historyStartSeq: finiteNumber(firstRecorded?.sequence) ?? undefined,
  };
}
