const MODEL_EVENT_TYPES = new Set([
  'model_step_start',
  'model_request',
  'model_first_output',
  'model_response',
  'model_step_finish',
]);

const TOOL_EVENT_TYPES = new Set(['tool_start', 'tool_end']);

const CATEGORY_LABELS = {
  context: '上下文',
  user: '用户',
  model: '模型',
  tool: '工具',
  permission: '权限',
  question: '提问',
  usage: '用量',
  progress: '进度',
  error: '错误',
  session: '会话',
};

function finiteTimestamp(value) {
  const number = Number(value);
  return Number.isFinite(number) && number > 0 ? number : null;
}

function safeJsonText(value) {
  if (value == null) return '';
  if (typeof value === 'string') return value;
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function compactText(value, maxLength = 180) {
  const text = safeJsonText(value).replace(/\s+/g, ' ').trim();
  return text.length > maxLength ? `${text.slice(0, maxLength - 1)}…` : text;
}

function recordOrder(record) {
  if (record?.sequence != null && Number.isFinite(Number(record.sequence))) {
    return [1, Number(record.sequence)];
  }
  return [0, Number(record?.legacy_index) || 0];
}

export function trajectoryRecordKey(record) {
  if (record?.sequence != null && Number.isFinite(Number(record.sequence))) {
    return `recorded:${record.sequence}`;
  }
  if (record?.legacy_index != null) return `legacy:${record.legacy_index}`;
  return `unknown:${record?.type || 'event'}:${safeJsonText(record?.payload)}`;
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

function makeTurn(id, ordinal, source = 'recorded') {
  return {
    id,
    ordinal,
    source,
    title: ordinal > 0 ? `轮次 ${ordinal}` : '会话事件',
    rows: [],
    startMs: null,
    endMs: null,
    durationMs: null,
    outcome: '',
    startRecord: null,
    endRecord: null,
  };
}

function categoryForType(type, payload) {
  if (type === 'legacy_context') return 'context';
  if (type === 'legacy_user_message') return 'user';
  if (type === 'legacy_model_response') return 'model';
  if (type === 'legacy_tool_result') return 'tool';
  if (type === 'message') {
    const role = payload?.role || '';
    if (role === 'user') return payload?.is_meta ? 'context' : 'user';
    if (role === 'system' || role === 'context') return 'context';
    if (role === 'error') return 'error';
    return 'model';
  }
  if (type.startsWith('permission_')) return 'permission';
  if (type.startsWith('question_')) return 'question';
  if (type === 'usage') return 'usage';
  if (type === 'agent_progress') return 'progress';
  if (type === 'error') return 'error';
  return 'session';
}

function labelForGenericRecord(type, payload, category) {
  if (category === 'user') return '用户消息';
  if (category === 'context') return payload?.role === 'system' ? '系统上下文' : '上下文';
  if (type === 'legacy_model_response') return '模型响应（旧记录）';
  if (type === 'legacy_tool_result') return '工具结果（旧记录）';
  if (type === 'permission_request') return '等待权限确认';
  if (type === 'permission_closed') return '权限确认结束';
  if (type === 'question_request') return '等待用户回答';
  if (type === 'question_closed') return '用户回答结束';
  if (type === 'usage') return 'Token 用量';
  if (type === 'agent_progress') return payload?.label || '执行进度';
  if (type === 'busy_changed') return payload?.busy ? '会话开始运行' : '会话转为空闲';
  if (type === 'done') return '会话完成';
  if (type === 'error') return '执行错误';
  if (type === 'message' && payload?.role === 'error') return '错误消息';
  if (type === 'message' && payload?.role === 'assistant') return '助手消息';
  return type || CATEGORY_LABELS[category] || '事件';
}

function previewForPayload(payload, category, type = '') {
  if (!payload || typeof payload !== 'object') return compactText(payload);
  if (type === 'busy_changed') return payload.busy ? '运行中' : '空闲';
  if (type === 'done') return compactText(payload.outcome || payload.status || '完成');
  if (category === 'user' || category === 'context' || category === 'model') {
    return compactText(
      payload.content || payload.reasoning_content || payload.message || payload.text || '',
    );
  }
  if (category === 'tool') return compactText(payload.output || payload.args || payload);
  if (category === 'error') {
    return compactText(payload.message || payload.error || payload.content || payload);
  }
  return compactText(payload.detail || payload.label || payload);
}

function makeGenericRow(record, turnId) {
  const payload = record.payload || {};
  const category = categoryForType(record.type || '', payload);
  const timestamp = finiteTimestamp(record.timestamp_ms);
  return {
    key: trajectoryRecordKey(record),
    turnId,
    category,
    badge: CATEGORY_LABELS[category] || '事件',
    label: labelForGenericRecord(record.type || '', payload, category),
    preview: previewForPayload(payload, category, record.type || ''),
    status: payload.status || payload.outcome || '',
    source: record.source || (record.sequence == null ? 'legacy' : 'recorded'),
    startMs: timestamp,
    endMs: timestamp,
    durationMs: null,
    rawRecords: [record],
    details: {
      summary: {
        type: record.type || '',
        category,
        source: record.source || (record.sequence == null ? 'legacy' : 'recorded'),
        status: payload.status || payload.outcome || null,
      },
      payload,
      result: category === 'tool' ? payload.output ?? null : null,
      schema: null,
      timing: {
        timestamp_ms: timestamp,
        started_at_ms: timestamp,
        completed_at_ms: timestamp,
        duration_ms: null,
      },
    },
  };
}

function ensureModelRow(turn, record, rowByKey) {
  const payload = record.payload || {};
  const step = Number(payload.step_index) || 0;
  const key = `model:${turn.id}:${step || trajectoryRecordKey(record)}`;
  let row = rowByKey.get(key);
  if (!row) {
    row = {
      key,
      turnId: turn.id,
      category: 'model',
      badge: '模型',
      label: step ? `模型步骤 ${step}` : '模型步骤',
      preview: '',
      status: '',
      source: record.source || 'recorded',
      startMs: null,
      endMs: null,
      firstOutputMs: null,
      durationMs: null,
      ttftMs: null,
      stepIndex: step,
      request: null,
      response: null,
      finish: null,
      rawRecords: [],
      details: {},
    };
    rowByKey.set(key, row);
    turn.rows.push(row);
  }
  row.rawRecords.push(record);
  const timestamp = finiteTimestamp(record.timestamp_ms);
  if (record.type === 'model_step_start') row.startMs = timestamp;
  if (record.type === 'model_request') {
    row.request = payload;
    row.startMs ??= timestamp;
    const identity = [payload.provider, payload.model].filter(Boolean).join(' / ');
    if (identity) row.label = `模型 · ${identity}`;
  }
  if (record.type === 'model_first_output') row.firstOutputMs = timestamp;
  if (record.type === 'model_response') {
    row.response = payload;
    row.endMs ??= timestamp;
    row.status = payload.status || payload.finish_reason || '';
    row.preview = compactText(
      payload.reasoning_content || payload.content || payload.tool_calls || '',
    );
  }
  if (record.type === 'model_step_finish') {
    row.finish = payload;
    row.endMs = timestamp ?? row.endMs;
    row.status = payload.reason || row.status;
  }
  return row;
}

function ensureToolRow(turn, record, rowByKey) {
  const payload = record.payload || {};
  const callId = payload.tool_call_id || '';
  const toolIndex = payload.tool_index ?? '';
  const fallback = `${payload.tool || 'tool'}:${toolIndex}:${trajectoryRecordKey(record)}`;
  const key = `tool:${turn.id}:${callId || fallback}`;
  let row = rowByKey.get(key);
  if (!row) {
    row = {
      key,
      turnId: turn.id,
      category: 'tool',
      badge: '工具',
      label: `工具 · ${payload.tool || '未知工具'}`,
      preview: '',
      status: '',
      source: record.source || 'recorded',
      startMs: null,
      endMs: null,
      durationMs: null,
      toolName: payload.tool || '',
      toolCallId: callId,
      start: null,
      end: null,
      schema: null,
      rawRecords: [],
      details: {},
    };
    rowByKey.set(key, row);
    turn.rows.push(row);
  }
  row.rawRecords.push(record);
  row.toolName ||= payload.tool || '';
  if (row.toolName) row.label = `工具 · ${row.toolName}`;
  if (record.type === 'tool_start') {
    row.start = payload;
    row.startMs = finiteTimestamp(payload.started_at_ms) || finiteTimestamp(record.timestamp_ms);
    row.preview = compactText(payload.args || payload.display || payload.command || '');
  } else {
    row.end = payload;
    row.endMs = finiteTimestamp(payload.completed_at_ms) || finiteTimestamp(record.timestamp_ms);
    row.startMs ??= finiteTimestamp(payload.started_at_ms);
    row.durationMs = Number.isFinite(Number(payload.duration_ms))
      ? Math.max(0, Number(payload.duration_ms))
      : null;
    row.status = payload.success === true
      ? 'completed'
      : (payload.success === false ? 'error' : payload.status || '');
    row.preview ||= compactText(payload.output || '');
  }
  return row;
}

function finalizeRow(row) {
  if ((row.category === 'model' || row.category === 'tool')
      && row.durationMs == null
      && row.startMs != null
      && row.endMs != null) {
    row.durationMs = Math.max(0, row.endMs - row.startMs);
  }
  if (row.category === 'model') {
    row.ttftMs = row.startMs != null && row.firstOutputMs != null
      ? Math.max(0, row.firstOutputMs - row.startMs)
      : null;
    row.details = {
      summary: {
        type: 'model_step',
        step_index: row.stepIndex || null,
        provider: row.request?.provider || row.response?.provider || null,
        model: row.request?.model || row.response?.model || null,
        status: row.status || null,
        finish_reason: row.response?.finish_reason || row.finish?.reason || null,
        usage: row.response?.usage || row.finish?.usage || null,
      },
      payload: row.request,
      result: row.response,
      schema: row.request?.tools || null,
      timing: {
        started_at_ms: row.startMs,
        first_output_at_ms: row.firstOutputMs,
        completed_at_ms: row.endMs,
        duration_ms: row.durationMs,
        ttft_ms: row.ttftMs,
      },
    };
  } else if (row.category === 'tool' && (row.start || row.end)) {
    row.details = {
      summary: {
        type: 'tool_call',
        tool: row.toolName || null,
        tool_call_id: row.toolCallId || null,
        status: row.status || null,
        success: row.end?.success ?? null,
        failure_stage: row.end?.failure_stage || null,
      },
      payload: row.start?.args ?? null,
      result: row.end ? {
        output: row.end.output ?? null,
        summary: row.end.summary ?? null,
        metadata: row.end.metadata ?? null,
        attachments: row.end.attachments ?? null,
        hunks: row.end.hunks ?? null,
      } : null,
      schema: row.schema,
      timing: {
        started_at_ms: row.startMs,
        completed_at_ms: row.endMs,
        duration_ms: row.durationMs,
      },
    };
  }
  const searchable = [
    row.badge,
    row.label,
    row.preview,
    row.status,
    safeJsonText(row.details),
  ].join(' ').toLocaleLowerCase();
  return { ...row, searchText: searchable };
}

export function buildTrajectoryViewModel(records = [], missingCapabilities = []) {
  const orderedRecords = mergeTrajectoryRecords([], records);
  const responseMessageIds = new Set(
    orderedRecords
      .filter((record) => record.type === 'model_response')
      .map((record) => record.payload?.message_id)
      .filter(Boolean),
  );
  const sessionTurn = makeTurn('session-events', 0, 'mixed');
  const turns = [];
  const turnById = new Map();
  const rowByKey = new Map();
  let currentTurn = sessionTurn;
  let nextTurnOrdinal = 1;

  const ensureTurn = (id, source = 'recorded') => {
    const safeId = id || `turn-${nextTurnOrdinal}`;
    let turn = turnById.get(safeId);
    if (!turn) {
      turn = makeTurn(safeId, nextTurnOrdinal++, source);
      turnById.set(safeId, turn);
      turns.push(turn);
    }
    return turn;
  };

  for (const record of orderedRecords) {
    const type = String(record.type || '');
    const payload = record.payload || {};
    if (type === 'turn_start') {
      currentTurn = ensureTurn(
        payload.turn_id || payload.user_message_id,
        record.source || 'recorded',
      );
      currentTurn.startRecord = record;
      currentTurn.startMs = finiteTimestamp(payload.started_at_ms)
        || finiteTimestamp(record.timestamp_ms);
      continue;
    }
    if (type === 'turn_end' || type === 'legacy_turn_end') {
      const turn = ensureTurn(
        payload.turn_id || payload.user_message_uuid,
        record.source || (type.startsWith('legacy_') ? 'legacy' : 'recorded'),
      );
      turn.endRecord = record;
      turn.startMs ??= finiteTimestamp(payload.started_at_ms);
      turn.endMs = finiteTimestamp(payload.completed_at_ms)
        || finiteTimestamp(record.timestamp_ms);
      turn.durationMs = Number.isFinite(Number(payload.duration_ms))
        ? Math.max(0, Number(payload.duration_ms))
        : (turn.startMs != null && turn.endMs != null
            ? Math.max(0, turn.endMs - turn.startMs)
            : null);
      turn.outcome = payload.outcome || payload.status || '';
      currentTurn = turn;
      continue;
    }

    if (type === 'legacy_user_message'
        || (type === 'message' && payload.role === 'user' && !payload.is_meta)) {
      const messageId = payload.uuid || payload.id || payload.message_id;
      if (currentTurn === sessionTurn
          || (messageId && currentTurn.id !== messageId && currentTurn.endRecord)) {
        currentTurn = ensureTurn(
          messageId || `legacy-${record.legacy_index ?? nextTurnOrdinal}`,
          record.source || (type.startsWith('legacy_') ? 'legacy' : 'recorded'),
        );
      }
      const row = makeGenericRow(record, currentTurn.id);
      currentTurn.rows.push(row);
      rowByKey.set(row.key, row);
      continue;
    }

    if (MODEL_EVENT_TYPES.has(type)) {
      ensureModelRow(currentTurn, record, rowByKey);
      continue;
    }
    if (TOOL_EVENT_TYPES.has(type)) {
      ensureToolRow(currentTurn, record, rowByKey);
      continue;
    }
    if (type === 'message'
        && payload.role === 'assistant'
        && responseMessageIds.has(payload.id)) {
      continue;
    }

    const target = currentTurn || sessionTurn;
    const row = makeGenericRow(record, target.id);
    target.rows.push(row);
    rowByKey.set(row.key, row);
  }

  const allTurns = sessionTurn.rows.length > 0 ? [sessionTurn, ...turns] : turns;
  for (const turn of allTurns) {
    let knownToolSchemas = new Map();
    turn.rows = turn.rows.map((row) => {
      if (row.category === 'model' && Array.isArray(row.request?.tools)) {
        knownToolSchemas = new Map();
        for (const tool of row.request.tools) {
          if (tool?.name) knownToolSchemas.set(tool.name, tool);
          if (tool?.native_name) knownToolSchemas.set(tool.native_name, tool);
        }
      }
      if (row.category === 'tool' && row.toolName) {
        row.schema = knownToolSchemas.get(row.toolName) || null;
      }
      return finalizeRow(row);
    });
    if (turn.startMs == null) {
      turn.startMs = turn.rows.find((row) => row.startMs != null)?.startMs ?? null;
    }
    if (turn.endMs == null) {
      const timedRows = turn.rows.filter((row) => row.endMs != null);
      turn.endMs = timedRows.length > 0
        ? timedRows[timedRows.length - 1].endMs
        : null;
    }
    if (turn.durationMs == null && turn.startMs != null && turn.endMs != null) {
      turn.durationMs = Math.max(0, turn.endMs - turn.startMs);
    }
    turn.searchText = [
      turn.title,
      turn.outcome,
      ...turn.rows.map((row) => row.searchText),
    ].join(' ').toLocaleLowerCase();
  }

  const rows = allTurns.flatMap((turn) => turn.rows);
  const timestamps = rows.flatMap((row) => [row.startMs, row.endMs])
    .filter((value) => value != null);
  const rangeStartMs = timestamps.length > 0 ? Math.min(...timestamps) : null;
  const rangeEndMs = timestamps.length > 0 ? Math.max(...timestamps) : null;
  return {
    turns: allTurns,
    rows,
    rowByKey: new Map(rows.map((row) => [row.key, row])),
    rangeStartMs,
    rangeEndMs,
    missingCapabilities: [...new Set(missingCapabilities || [])],
  };
}

export function trajectoryMatches(model, query) {
  const normalized = String(query || '').trim().toLocaleLowerCase();
  if (!normalized) return [];
  return model.rows
    .filter((row) => row.searchText.includes(normalized))
    .map((row) => row.key);
}

export function trajectoryTimelineSegments(model, mode = 'duration') {
  if (!model) return [];
  if (mode === 'turns') {
    return model.turns
      .filter((turn) => turn.startMs != null || turn.endMs != null)
      .map((turn) => ({
        key: `turn:${turn.id}`,
        label: turn.title,
        category: 'turn',
        startMs: turn.startMs ?? turn.endMs,
        endMs: turn.endMs ?? turn.startMs,
        durationMs: turn.durationMs,
        turnId: turn.id,
      }));
  }
  const rows = mode === 'calls'
    ? model.rows.filter((row) => row.category === 'model' || row.category === 'tool')
    : model.rows;
  return rows
    .filter((row) => row.startMs != null || row.endMs != null)
    .map((row) => ({
      key: row.key,
      label: row.label,
      category: row.category,
      startMs: row.startMs ?? row.endMs,
      endMs: row.endMs ?? row.startMs,
      durationMs: row.durationMs,
      turnId: row.turnId,
    }));
}

export function formatTrajectoryDuration(durationMs) {
  if (durationMs == null || !Number.isFinite(Number(durationMs))) return '未记录';
  const value = Math.max(0, Number(durationMs));
  if (value < 1000) return `${Math.round(value)} ms`;
  if (value < 60000) return `${(value / 1000).toFixed(value < 10000 ? 2 : 1)} s`;
  const minutes = Math.floor(value / 60000);
  const seconds = Math.round((value % 60000) / 1000);
  return `${minutes}m ${seconds}s`;
}

export function formatTrajectoryTimestamp(timestampMs) {
  if (timestampMs == null || !Number.isFinite(Number(timestampMs))) return '未记录';
  return new Date(Number(timestampMs)).toLocaleString(undefined, {
    hour12: false,
  });
}
