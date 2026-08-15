import { useVirtualizer } from '@tanstack/react-virtual';
import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
} from 'react';

import {
  buildTrajectoryViewModel,
  formatTrajectoryDuration,
  formatTrajectoryTimestamp,
  mergeTrajectoryRecords,
  trajectoryMatches,
  trajectoryTimelineSegments,
} from '../../lib/trajectoryModel.js';
import { VsIcon } from '../Icon.jsx';
import './TrajectoryView.css';

const MODE_OPTIONS = [
  { id: 'duration', label: 'Duration' },
  { id: 'turns', label: 'Turns' },
  { id: 'calls', label: 'Calls' },
];

const DETAIL_TABS = [
  { id: 'summary', label: '摘要' },
  { id: 'payload', label: '载荷' },
  { id: 'result', label: '结果' },
  { id: 'schema', label: '结构' },
  { id: 'timing', label: '时间' },
];

const SOURCE_LABELS = {
  recorded: '精确轨迹',
  legacy: '旧会话投影',
  mixed: '新旧混合',
  empty: '暂无记录',
};

const MISSING_LABELS = {
  model_request: '模型请求',
  model_step_timing: '模型步骤耗时',
  ttft: '首个输出时间',
  tool_timing: '工具耗时',
  tool_schema: '工具结构',
};

const LANE_LABELS = {
  input: 'Input',
  model: 'Model',
  tools: 'Tools',
  other: 'Other',
  turns: 'Turns',
};

function errorMessage(error) {
  if (!error) return '';
  if (typeof error?.body?.message === 'string') return error.body.message;
  if (typeof error?.body?.error === 'string') return error.body.error;
  return error.message || String(error);
}

function laneForSegment(segment, mode) {
  if (mode === 'turns') return 'turns';
  if (segment.category === 'user' || segment.category === 'context') return 'input';
  if (segment.category === 'model') return 'model';
  if (segment.category === 'tool') return 'tools';
  return 'other';
}

function TrajectoryOverview({ model, mode, onSelect }) {
  const segments = useMemo(
    () => trajectoryTimelineSegments(model, mode),
    [model, mode],
  );
  const lanes = useMemo(() => {
    const grouped = new Map();
    for (const segment of segments) {
      const lane = laneForSegment(segment, mode);
      if (!grouped.has(lane)) grouped.set(lane, []);
      grouped.get(lane).push(segment);
    }
    const order = mode === 'turns'
      ? ['turns']
      : (mode === 'calls' ? ['model', 'tools'] : ['input', 'model', 'tools', 'other']);
    return order
      .filter((lane) => grouped.has(lane))
      .map((lane) => ({ id: lane, segments: grouped.get(lane) }));
  }, [mode, segments]);
  const start = segments.length > 0
    ? Math.min(...segments.map((segment) => segment.startMs))
    : null;
  const end = segments.length > 0
    ? Math.max(...segments.map((segment) => segment.endMs))
    : null;
  const range = start != null && end != null ? Math.max(1, end - start) : 1;

  if (segments.length === 0) {
    return <div className="ace-trajectory-overview-empty">时间边界未记录</div>;
  }
  return (
    <div className="ace-trajectory-overview" aria-label="会话轨迹时间线">
      {lanes.map((lane) => (
        <div key={lane.id} className="ace-trajectory-lane">
          <div className="ace-trajectory-lane-label">{LANE_LABELS[lane.id]}</div>
          <div className="ace-trajectory-lane-track">
            {lane.segments.map((segment) => {
              const left = ((segment.startMs - start) / range) * 100;
              const width = Math.max(.35, ((segment.endMs - segment.startMs) / range) * 100);
              const timingLabel = segment.durationMs != null
                ? formatTrajectoryDuration(segment.durationMs)
                : formatTrajectoryTimestamp(segment.startMs);
              return (
                <button
                  key={segment.key}
                  type="button"
                  className="ace-trajectory-segment"
                  data-category={segment.category}
                  style={{ left: `${left}%`, width: `${width}%` }}
                  title={`${segment.label} · ${timingLabel}`}
                  aria-label={`${segment.label}，${timingLabel}`}
                  onClick={() => onSelect(segment)}
                />
              );
            })}
          </div>
        </div>
      ))}
    </div>
  );
}

function displayValue(value) {
  if (value == null || value === '') return '未记录';
  if (typeof value === 'boolean') return value ? '是' : '否';
  if (typeof value === 'object') return JSON.stringify(value);
  return String(value);
}

function SummaryGrid({ value }) {
  if (!value || typeof value !== 'object' || Object.keys(value).length === 0) {
    return <div className="ace-trajectory-empty">未记录</div>;
  }
  return (
    <div className="ace-trajectory-summary-grid">
      {Object.entries(value).map(([key, item]) => (
        <div key={key} style={{ display: 'contents' }}>
          <div className="ace-trajectory-summary-key">{key}</div>
          <div className="ace-trajectory-summary-value">{displayValue(item)}</div>
        </div>
      ))}
    </div>
  );
}

function DetailValue({ tab, value }) {
  if (tab === 'summary') return <SummaryGrid value={value} />;
  if (value == null || value === '' || (Array.isArray(value) && value.length === 0)) {
    return <div className="ace-trajectory-empty">未记录</div>;
  }
  const text = typeof value === 'string' ? value : JSON.stringify(value, null, 2);
  return <pre className="ace-trajectory-json">{text}</pre>;
}

function TrajectoryDetails({ row, tab, onTabChange, onClose }) {
  if (!row) {
    return (
      <aside className="ace-trajectory-details" aria-label="轨迹详情">
        <div className="ace-trajectory-empty">选择一条轨迹查看原始详情</div>
      </aside>
    );
  }
  return (
    <aside className="ace-trajectory-details" aria-label="轨迹详情">
      <div className="ace-trajectory-details-header">
        <span className="ace-trajectory-badge">{row.badge}</span>
        <div className="ace-trajectory-details-title">{row.label}</div>
        <button
          type="button"
          className="ace-trajectory-details-close"
          onClick={onClose}
          title="关闭详情"
          aria-label="关闭轨迹详情"
        >
          <VsIcon name="close" size={13} />
        </button>
      </div>
      <div className="ace-trajectory-details-tabs" role="tablist" aria-label="轨迹详情分类">
        {DETAIL_TABS.map((item) => (
          <button
            key={item.id}
            type="button"
            role="tab"
            aria-selected={tab === item.id}
            data-active={tab === item.id ? 'true' : 'false'}
            className="ace-trajectory-details-tab"
            onClick={() => onTabChange(item.id)}
          >
            {item.label}
          </button>
        ))}
      </div>
      <div className="ace-trajectory-details-body" role="tabpanel">
        <DetailValue tab={tab} value={row.details?.[tab]} />
      </div>
    </aside>
  );
}

function turnDetailRow(turn) {
  return {
    key: `turn-detail:${turn.id}`,
    turnId: turn.id,
    category: 'session',
    badge: '轮次',
    label: turn.title,
    details: {
      summary: {
        turn_id: turn.id,
        source: turn.source,
        outcome: turn.outcome || null,
        event_count: turn.rows.length,
      },
      payload: turn.startRecord?.payload ?? null,
      result: turn.endRecord?.payload ?? null,
      schema: null,
      timing: {
        started_at_ms: turn.startMs,
        completed_at_ms: turn.endMs,
        duration_ms: turn.durationMs,
      },
    },
  };
}

export function TrajectoryView({
  api,
  sessionId,
  workspaceHash = '',
  active = true,
  busy = false,
}) {
  const [records, setRecords] = useState([]);
  const [source, setSource] = useState('empty');
  const [missingCapabilities, setMissingCapabilities] = useState([]);
  const [diagnostics, setDiagnostics] = useState(null);
  const [hasMore, setHasMore] = useState(false);
  const [loading, setLoading] = useState(false);
  const [loadingMore, setLoadingMore] = useState(false);
  const [error, setError] = useState('');
  const [mode, setMode] = useState('duration');
  const [query, setQuery] = useState('');
  const [matchIndex, setMatchIndex] = useState(-1);
  const [collapsedTurns, setCollapsedTurns] = useState(() => new Set());
  const [selectedKey, setSelectedKey] = useState('');
  const [selectedTurn, setSelectedTurn] = useState(null);
  const [detailsTab, setDetailsTab] = useState('summary');
  const [pendingScrollKey, setPendingScrollKey] = useState('');
  const listRef = useRef(null);
  const fetchPageRef = useRef(null);
  const generationRef = useRef(0);
  const busyRef = useRef(busy);
  const previousBusyRef = useRef(busy);

  useEffect(() => {
    const generation = ++generationRef.current;
    let requestInFlight = false;
    let pollQueued = false;
    let preciseAfter = 0;
    let legacyAfter = 0;
    let legacyTotal = 0;
    let recordedHasMore = false;
    let legacyHasMore = false;

    setRecords([]);
    setSource('empty');
    setMissingCapabilities([]);
    setDiagnostics(null);
    setHasMore(false);
    setError('');
    setSelectedKey('');
    setSelectedTurn(null);
    setCollapsedTurns(new Set());
    setQuery('');
    setMatchIndex(-1);
    if (!active || !api || !sessionId) {
      fetchPageRef.current = null;
      return undefined;
    }

    const requestPage = async (kind = 'poll') => {
      if (requestInFlight) {
        if (kind === 'poll') pollQueued = true;
        return;
      }
      if (kind === 'poll' && recordedHasMore) return;
      requestInFlight = true;
      if (kind === 'initial') setLoading(true);
      if (kind === 'more') setLoadingMore(true);
      try {
        const response = await api.getTrajectory(sessionId, {
          after: kind === 'initial' ? 0 : preciseAfter,
          legacyAfter: kind === 'initial'
            ? 0
            : (kind === 'poll' ? legacyTotal : legacyAfter),
          limit: 250,
          workspaceHash,
        });
        if (generationRef.current !== generation) return;
        const incoming = Array.isArray(response?.records) ? response.records : [];
        setRecords((previous) => mergeTrajectoryRecords(previous, incoming));
        preciseAfter = Math.max(preciseAfter, Number(response?.next_after) || 0);
        if (kind !== 'poll') {
          legacyAfter = Math.max(legacyAfter, Number(response?.legacy_next_after) || 0);
          legacyHasMore = !!response?.legacy_has_more;
        }
        legacyTotal = Math.max(legacyTotal, Number(response?.legacy_total) || 0);
        recordedHasMore = !!response?.recorded_has_more;
        setHasMore(recordedHasMore || legacyHasMore);
        setSource(response?.source || 'empty');
        setMissingCapabilities(
          Array.isArray(response?.missing_capabilities)
            ? response.missing_capabilities
            : [],
        );
        setDiagnostics(response?.diagnostics || null);
        setError('');
      } catch (requestError) {
        if (generationRef.current === generation) {
          setError(errorMessage(requestError));
        }
      } finally {
        if (generationRef.current === generation) {
          requestInFlight = false;
          setLoading(false);
          setLoadingMore(false);
          if (pollQueued) {
            pollQueued = false;
            window.queueMicrotask(() => requestPage('poll'));
          }
        }
      }
    };

    fetchPageRef.current = requestPage;
    requestPage('initial');
    const timer = window.setInterval(() => {
      if (busyRef.current) requestPage('poll');
    }, 1500);
    return () => {
      if (generationRef.current === generation) {
        generationRef.current += 1;
      }
      window.clearInterval(timer);
      if (fetchPageRef.current === requestPage) fetchPageRef.current = null;
    };
  }, [active, api, sessionId, workspaceHash]);

  useEffect(() => {
    const changed = previousBusyRef.current !== busy;
    busyRef.current = busy;
    previousBusyRef.current = busy;
    if (changed && active && api && sessionId) {
      fetchPageRef.current?.('poll');
    }
  }, [active, api, busy, sessionId]);

  const model = useMemo(
    () => buildTrajectoryViewModel(records, missingCapabilities),
    [missingCapabilities, records],
  );
  const matches = useMemo(() => trajectoryMatches(model, query), [model, query]);
  const matchSet = useMemo(() => new Set(matches), [matches]);
  const visibleItems = useMemo(() => {
    const items = [];
    for (const turn of model.turns) {
      items.push({ kind: 'turn', key: `turn:${turn.id}`, turn });
      if (!collapsedTurns.has(turn.id)) {
        for (const row of turn.rows) items.push({ kind: 'row', key: row.key, row });
      }
    }
    return items;
  }, [collapsedTurns, model]);
  const virtualizer = useVirtualizer({
    count: visibleItems.length,
    getScrollElement: () => listRef.current,
    estimateSize: (index) => visibleItems[index]?.kind === 'turn' ? 35 : 51,
    overscan: 12,
  });

  useEffect(() => {
    if (!pendingScrollKey) return;
    const index = visibleItems.findIndex((item) => item.key === pendingScrollKey);
    if (index < 0) return;
    virtualizer.scrollToIndex(index, { align: 'center' });
    setPendingScrollKey('');
  }, [pendingScrollKey, virtualizer, visibleItems]);

  useEffect(() => {
    if (matches.length === 0) {
      setMatchIndex(-1);
    } else if (matchIndex >= matches.length) {
      setMatchIndex(0);
    }
  }, [matchIndex, matches]);

  const selectedRow = selectedTurn
    ? turnDetailRow(selectedTurn)
    : model.rowByKey.get(selectedKey) || null;

  const jumpToRow = useCallback((key) => {
    const row = model.rowByKey.get(key);
    if (!row) return;
    setCollapsedTurns((previous) => {
      if (!previous.has(row.turnId)) return previous;
      const next = new Set(previous);
      next.delete(row.turnId);
      return next;
    });
    setSelectedTurn(null);
    setSelectedKey(key);
    setDetailsTab('summary');
    setPendingScrollKey(key);
  }, [model]);

  const moveMatch = useCallback((delta) => {
    if (matches.length === 0) return;
    const next = matchIndex < 0
      ? (delta >= 0 ? 0 : matches.length - 1)
      : (matchIndex + delta + matches.length) % matches.length;
    setMatchIndex(next);
    jumpToRow(matches[next]);
  }, [jumpToRow, matchIndex, matches]);

  const selectTimelineSegment = useCallback((segment) => {
    if (segment.key.startsWith('turn:')) {
      const turn = model.turns.find((item) => item.id === segment.turnId);
      if (!turn) return;
      setSelectedKey('');
      setSelectedTurn(turn);
      setCollapsedTurns((previous) => {
        if (!previous.has(turn.id)) return previous;
        const next = new Set(previous);
        next.delete(turn.id);
        return next;
      });
      setPendingScrollKey(`turn:${turn.id}`);
      return;
    }
    jumpToRow(segment.key);
  }, [jumpToRow, model.turns]);

  const missingText = missingCapabilities
    .map((capability) => MISSING_LABELS[capability] || capability)
    .join('、');
  const recoveryNotice = diagnostics && (
    diagnostics.malformed_complete_records > 0
      || diagnostics.ignored_partial_tail
      || diagnostics.recovered_unterminated_record
  );

  return (
    <section className="ace-trajectory-shell" aria-label="会话轨迹" data-source={source}>
      <div className="ace-trajectory-toolbar">
        <div className="ace-trajectory-mode-group" role="group" aria-label="轨迹时间线模式">
          {MODE_OPTIONS.map((option) => (
            <button
              key={option.id}
              type="button"
              className="ace-trajectory-mode-button"
              data-active={mode === option.id ? 'true' : 'false'}
              aria-pressed={mode === option.id}
              onClick={() => setMode(option.id)}
            >
              {option.label}
            </button>
          ))}
        </div>
        <div className="ace-trajectory-search">
          <VsIcon name="search" size={13} />
          <input
            value={query}
            onChange={(event) => {
              setQuery(event.target.value);
              setMatchIndex(-1);
            }}
            onKeyDown={(event) => {
              if (event.key !== 'Enter') return;
              event.preventDefault();
              moveMatch(event.shiftKey ? -1 : 1);
            }}
            placeholder="搜索轨迹内容"
            aria-label="搜索轨迹内容"
          />
          {query && (
            <span className="ace-trajectory-search-count">
              {matches.length > 0 ? `${Math.max(0, matchIndex) + 1}/${matches.length}` : '0/0'}
            </span>
          )}
          <button
            type="button"
            className="ace-trajectory-search-action"
            disabled={matches.length === 0}
            onClick={() => moveMatch(-1)}
            title="上一个匹配"
            aria-label="上一个轨迹匹配"
          >
            <VsIcon name="glyphUp" size={11} />
          </button>
          <button
            type="button"
            className="ace-trajectory-search-action"
            disabled={matches.length === 0}
            onClick={() => moveMatch(1)}
            title="下一个匹配"
            aria-label="下一个轨迹匹配"
          >
            <VsIcon name="glyphDown" size={11} />
          </button>
        </div>
      </div>

      <TrajectoryOverview model={model} mode={mode} onSelect={selectTimelineSegment} />

      {missingText && (
        <div className="ace-trajectory-notice" role="note">
          旧会话的以下信息未记录：{missingText}。界面不会根据相邻消息推测这些数据。
        </div>
      )}
      {recoveryNotice && (
        <div className="ace-trajectory-notice" role="status">
          轨迹文件包含不完整或损坏记录；已跳过异常部分并保留其余有效数据。
        </div>
      )}
      {error && (
        <div className="ace-trajectory-notice ace-trajectory-error" role="alert">
          轨迹读取失败：{error}。正常对话不受影响，将自动重试。
        </div>
      )}

      <div
        className="ace-trajectory-content"
        data-details-open={selectedRow ? 'true' : 'false'}
      >
        <div className="ace-trajectory-list-pane">
          <div className="ace-trajectory-list-meta">
            <span className="ace-trajectory-source-pill">{SOURCE_LABELS[source] || source}</span>
            <span>{model.turns.length} 个轮次 · {model.rows.length} 条事件</span>
          </div>
          {loading && model.rows.length === 0 ? (
            <div className="ace-trajectory-empty">正在读取会话轨迹…</div>
          ) : model.turns.length === 0 ? (
            <div className="ace-trajectory-empty">
              这个会话还没有可展示的轨迹。发送消息后会在这里实时出现。
            </div>
          ) : (
            <div ref={listRef} className="ace-trajectory-list-scroll">
              <div
                className="ace-trajectory-virtual-canvas"
                style={{ height: virtualizer.getTotalSize() }}
              >
                {virtualizer.getVirtualItems().map((virtualItem) => {
                  const item = visibleItems[virtualItem.index];
                  if (!item) return null;
                  return (
                    <div
                      key={item.key}
                      ref={virtualizer.measureElement}
                      data-index={virtualItem.index}
                      className="ace-trajectory-virtual-item"
                      style={{ transform: `translateY(${virtualItem.start}px)` }}
                    >
                      {item.kind === 'turn' ? (
                        <button
                          type="button"
                          className="ace-trajectory-turn-row"
                          onClick={() => {
                            setCollapsedTurns((previous) => {
                              const next = new Set(previous);
                              if (next.has(item.turn.id)) next.delete(item.turn.id);
                              else next.add(item.turn.id);
                              return next;
                            });
                            setSelectedKey('');
                            setSelectedTurn(item.turn);
                            setDetailsTab('summary');
                          }}
                          aria-expanded={!collapsedTurns.has(item.turn.id)}
                        >
                          <VsIcon
                            name={collapsedTurns.has(item.turn.id) ? 'expandRight' : 'expandDown'}
                            size={11}
                          />
                          <span className="ace-trajectory-turn-title">{item.turn.title}</span>
                          {item.turn.outcome && (
                            <span className="ace-trajectory-turn-meta">{item.turn.outcome}</span>
                          )}
                          <span className="ace-trajectory-turn-meta">
                            {formatTrajectoryDuration(item.turn.durationMs)}
                          </span>
                        </button>
                      ) : (
                        <button
                          type="button"
                          className="ace-trajectory-event-row"
                          data-category={item.row.category}
                          data-selected={selectedKey === item.row.key ? 'true' : 'false'}
                          data-match={matchSet.has(item.row.key) ? 'true' : 'false'}
                          onClick={() => {
                            setSelectedTurn(null);
                            setSelectedKey(item.row.key);
                            setDetailsTab('summary');
                          }}
                        >
                          <span className="ace-trajectory-badge">{item.row.badge}</span>
                          <span className="ace-trajectory-event-copy">
                            <span className="ace-trajectory-event-label">{item.row.label}</span>
                            <span className="ace-trajectory-event-preview">
                              {item.row.preview || item.row.status || '—'}
                            </span>
                          </span>
                          <span className="ace-trajectory-event-time">
                            {item.row.durationMs != null
                              ? formatTrajectoryDuration(item.row.durationMs)
                              : formatTrajectoryTimestamp(item.row.startMs)}
                          </span>
                        </button>
                      )}
                    </div>
                  );
                })}
              </div>
            </div>
          )}
          {hasMore && (
            <div className="ace-trajectory-list-footer">
              <button
                type="button"
                className="ace-trajectory-load-more"
                disabled={loadingMore}
                onClick={() => fetchPageRef.current?.('more')}
              >
                {loadingMore ? '正在加载…' : '加载更早/后续记录'}
              </button>
            </div>
          )}
        </div>

        {selectedRow && (
          <button
            type="button"
            className="ace-trajectory-details-backdrop"
            aria-label="关闭轨迹详情"
            onClick={() => {
              setSelectedKey('');
              setSelectedTurn(null);
            }}
          />
        )}
        {selectedRow && (
          <TrajectoryDetails
            row={selectedRow}
            tab={detailsTab}
            onTabChange={setDetailsTab}
            onClose={() => {
              setSelectedKey('');
              setSelectedTurn(null);
            }}
          />
        )}
      </div>
    </section>
  );
}
