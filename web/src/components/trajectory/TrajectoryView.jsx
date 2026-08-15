import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import {
  appendTrajectoryPartialLayout,
  deriveTrajectoryLayout,
} from './deepseek/layout.ts';
import {
  buildDeepSeekTrajectory,
  mergeTrajectoryRecords,
  resolveDeepSeekTrajectoryPartial,
  trajectoryRecordKey,
} from '../../lib/trajectoryModel.js';
import { TrajectorySearchIndex } from './deepseek/trajectory-search-index.ts';
import { trajectoryRecordId } from './deepseek/trajectory-record.ts';
import { TrajectoryTable } from './deepseek/TrajectoryTable.tsx';
import { TrajectoryTimeline } from './deepseek/TrajectoryTimeline.tsx';
import { TrajectoryToolbar } from './deepseek/TrajectoryToolbar.tsx';
import { trajectoryTimelineFocusIndexes } from './deepseek/timeline.ts';
import css from './deepseek/views.module.css';
import './deepseek/theme.css';

const EMPTY_TURN_IDS = new Set();
const EMPTY_RECORD_IDS = new Set();
const PAGE_LIMIT = 250;
const POLL_INTERVAL_MS = 1500;
const SEARCH_INDEX_THROTTLE_MS = 3000;
const DURATION_STORAGE_KEY = 'dsh.trajectory.duration';

function readDurationPreference() {
  if (typeof localStorage === 'undefined') return false;
  try {
    const raw = localStorage.getItem(DURATION_STORAGE_KEY);
    return raw === null ? false : JSON.parse(raw) === true;
  } catch (error) {
    console.error(`snapshot store '${DURATION_STORAGE_KEY}' rehydration failed:`, error);
    return false;
  }
}

function writeDurationPreference(value) {
  if (typeof localStorage === 'undefined') return;
  try {
    localStorage.setItem(DURATION_STORAGE_KEY, JSON.stringify(value));
  } catch (error) {
    console.error(`snapshot store '${DURATION_STORAGE_KEY}' persistence failed:`, error);
  }
}

function lastCellIndex(turns) {
  let last = 0;
  for (const turn of turns) {
    for (const group of turn.groups) {
      for (const cell of group.cells) {
        last = Math.max(last, cell.index);
      }
    }
  }
  return last;
}

function timelineBlock(block) {
  switch (block.kind) {
    case 'text': return { kind: 'text', text: '' };
    case 'reasoning': return { kind: 'reasoning', text: '' };
    case 'image': return block;
    case 'tool-call': return {
      kind: 'tool-call',
      callId: block.callId,
      name: block.name,
      argsRaw: '',
    };
    default: return { kind: 'other', block: null };
  }
}

function partialStructureSignature(partial) {
  if (partial === null) return '';
  return partial.blocks.map((block) => (block.kind === 'tool-call'
    ? `${block.kind}:${block.callId}:${block.name}`
    : block.kind)).join('\u0000');
}

export function TrajectoryView({
  api,
  sessionId,
  workspaceHash = '',
  active = true,
  busy = false,
  livePartial = null,
  inspectCallId = null,
  onInspectApplied,
}) {
  const [records, setRecords] = useState([]);
  const [historyLoading, setHistoryLoading] = useState(false);
  const [olderHistoryLoading, setOlderHistoryLoading] = useState(false);
  const [hasOlderRecords, setHasOlderRecords] = useState(false);
  const [collapsedTurns, setCollapsedTurns] = useState(EMPTY_TURN_IDS);
  const [collapsedAssistants, setCollapsedAssistants] = useState(EMPTY_RECORD_IDS);
  const [timelineSelection, setTimelineSelection] = useState(null);
  const [actualDuration, setActualDuration] = useState(readDurationPreference);
  const [actualTime, setActualTime] = useState(false);
  const [searchQuery, setSearchQuery] = useState('');
  const [searchIndex] = useState(() => new TrajectorySearchIndex());
  const [searchRevision, setSearchRevision] = useState(0);
  const searchIndexTimer = useRef(null);
  const searchIndexInitialized = useRef(false);
  const [selectedTimelineIndex, setSelectedTimelineIndex] = useState(null);
  const [timelineRecordSelection, setTimelineRecordSelection] = useState(null);
  const [timelineRecordFocus, setTimelineRecordFocus] = useState(null);
  const generationRef = useRef(0);
  const busyRef = useRef(busy);
  const previousBusyRef = useRef(busy);
  const pollRef = useRef(null);
  const loadOlderRef = useRef(() => Promise.resolve(false));

  useEffect(() => {
    const generation = ++generationRef.current;
    let requestInFlight = false;
    let pollQueued = false;
    let preciseAfter = 0;
    let legacyAfter = 0;
    let preciseBefore = null;
    let legacyBefore = null;
    let recordedHasOlder = false;
    let legacyHasOlder = false;
    let olderRequest = null;
    const knownRecordKeys = new Set();

    setRecords([]);
    setHistoryLoading(false);
    setOlderHistoryLoading(false);
    setHasOlderRecords(false);
    setCollapsedTurns(EMPTY_TURN_IDS);
    setCollapsedAssistants(EMPTY_RECORD_IDS);
    setTimelineSelection(null);
    setSearchQuery('');
    setSelectedTimelineIndex(null);
    setTimelineRecordSelection(null);
    setTimelineRecordFocus(null);

    if (!active || !api || !sessionId) {
      pollRef.current = null;
      loadOlderRef.current = () => Promise.resolve(false);
      return undefined;
    }

    const mergeIncoming = (incoming) => {
      let advanced = false;
      for (const record of incoming) {
        const key = trajectoryRecordKey(record);
        if (knownRecordKeys.has(key)) continue;
        knownRecordKeys.add(key);
        advanced = true;
      }
      if (advanced) {
        setRecords((previous) => mergeTrajectoryRecords(previous, incoming));
      }
      return advanced;
    };

    const updateOlderState = (response) => {
      const firstSequence = Number(response?.first_sequence);
      const firstLegacyIndex = Number(response?.legacy_first_index);
      preciseBefore = Number.isFinite(firstSequence) ? firstSequence : 0;
      legacyBefore = Number.isFinite(firstLegacyIndex) ? firstLegacyIndex : 0;
      recordedHasOlder = !!response?.recorded_has_older;
      legacyHasOlder = !!response?.legacy_has_older;
      setHasOlderRecords(recordedHasOlder || legacyHasOlder);
    };

    const requestRecords = async (kind = 'poll') => {
      if (requestInFlight) {
        if (kind === 'poll') pollQueued = true;
        return;
      }
      requestInFlight = true;
      if (kind === 'initial') setHistoryLoading(true);
      try {
        if (kind === 'initial') {
          const response = await api.getTrajectory(sessionId, {
            tail: true,
            limit: PAGE_LIMIT,
            workspaceHash,
          });
          if (generationRef.current !== generation) return;
          const incoming = Array.isArray(response?.records) ? response.records : [];
          mergeIncoming(incoming);
          preciseAfter = Math.max(
            Number(response?.recorded_latest_sequence) || 0,
            Number(response?.next_after) || 0,
          );
          legacyAfter = Math.max(
            Number(response?.legacy_total) || 0,
            Number(response?.legacy_next_after) || 0,
          );
          updateOlderState(response);
          return;
        }

        let more = true;
        do {
          const response = await api.getTrajectory(sessionId, {
            after: preciseAfter,
            legacyAfter,
            limit: PAGE_LIMIT,
            workspaceHash,
          });
          if (generationRef.current !== generation) return;
          const incoming = Array.isArray(response?.records) ? response.records : [];
          mergeIncoming(incoming);
          preciseAfter = Math.max(preciseAfter, Number(response?.next_after) || 0);
          legacyAfter = Math.max(legacyAfter, Number(response?.legacy_next_after) || 0);
          more = !!response?.recorded_has_more || !!response?.legacy_has_more;
        } while (more && generationRef.current === generation);
      } catch (error) {
        console.error('Unable to load trajectory', error);
      } finally {
        if (generationRef.current === generation) {
          requestInFlight = false;
          setHistoryLoading(false);
          if (pollQueued) {
            pollQueued = false;
            window.queueMicrotask(() => requestRecords('poll'));
          }
        }
      }
    };

    const loadOlder = () => {
      if (olderRequest !== null) return olderRequest;
      if (!recordedHasOlder && !legacyHasOlder) return Promise.resolve(false);

      setOlderHistoryLoading(true);
      const options = {
        tail: true,
        limit: PAGE_LIMIT,
        workspaceHash,
        before: recordedHasOlder ? (preciseBefore ?? 0) : 0,
        legacyBefore: legacyHasOlder ? (legacyBefore ?? 0) : 0,
      };
      olderRequest = api.getTrajectory(sessionId, options)
        .then((response) => {
          if (generationRef.current !== generation) return false;
          const incoming = Array.isArray(response?.records) ? response.records : [];
          const advanced = mergeIncoming(incoming);
          updateOlderState(response);
          return advanced;
        })
        .catch((error) => {
          console.error('Unable to load earlier trajectory', error);
          return false;
        })
        .finally(() => {
          olderRequest = null;
          if (generationRef.current === generation) setOlderHistoryLoading(false);
        });
      return olderRequest;
    };

    pollRef.current = requestRecords;
    loadOlderRef.current = loadOlder;
    requestRecords('initial');
    const timer = window.setInterval(() => {
      if (busyRef.current) requestRecords('poll');
    }, POLL_INTERVAL_MS);
    return () => {
      window.clearInterval(timer);
      if (generationRef.current === generation) generationRef.current += 1;
      if (pollRef.current === requestRecords) pollRef.current = null;
      if (loadOlderRef.current === loadOlder) {
        loadOlderRef.current = () => Promise.resolve(false);
      }
    };
  }, [active, api, sessionId, workspaceHash]);

  useEffect(() => {
    const changed = previousBusyRef.current !== busy;
    busyRef.current = busy;
    previousBusyRef.current = busy;
    if (changed && active && api && sessionId) pollRef.current?.('poll');
  }, [active, api, busy, sessionId]);

  const projection = useMemo(() => buildDeepSeekTrajectory(records), [records]);
  const {
    nodes, runningCalls, requests, callSchemas,
  } = projection;
  const partial = useMemo(
    () => resolveDeepSeekTrajectoryPartial(projection, livePartial),
    [livePartial, projection],
  );
  const partialTurn = partial?.turn ?? null;
  const partialStep = partial?.step ?? null;
  const finalized = useMemo(() => {
    const turns = deriveTrajectoryLayout({
      nodes,
      partial: partialTurn === null || partialStep === null
        ? null
        : { turn: partialTurn, step: partialStep, blocks: [] },
      runningCalls,
      requests,
      callSchemas,
    });
    return { turns, lastIndex: lastCellIndex(turns) };
  }, [callSchemas, nodes, partialStep, partialTurn, requests, runningCalls]);
  const timelinePartialSignature = partialStructureSignature(partial);
  const timelinePartial = useMemo(() => (partial === null
    ? null
    : {
        turn: partial.turn,
        step: partial.step,
        blocks: partial.blocks.map((block) => timelineBlock(block)),
      }), [partialStep, partialTurn, timelinePartialSignature]);
  const timelineTurns = useMemo(
    () => appendTrajectoryPartialLayout(finalized.turns, timelinePartial, finalized.lastIndex),
    [finalized, timelinePartial],
  );
  const timelineMode = actualDuration
    ? (actualTime ? 'actual' : 'duration')
    : (actualTime ? 'time' : 'sequence');

  const partialSearchTurns = useMemo(
    () => appendTrajectoryPartialLayout([], partial, finalized.lastIndex),
    [finalized.lastIndex, partial],
  );
  const searchLayouts = useMemo(
    () => [finalized.turns, partialSearchTurns],
    [finalized, partialSearchTurns],
  );
  const latestSearchLayouts = useRef(searchLayouts);
  latestSearchLayouts.current = searchLayouts;
  useEffect(() => {
    if (!searchIndexInitialized.current) {
      searchIndexInitialized.current = true;
      if (searchIndex.update(searchLayouts)) {
        setSearchRevision((revision) => revision + 1);
      }
      return;
    }
    if (searchIndexTimer.current !== null) return;
    searchIndexTimer.current = window.setTimeout(() => {
      searchIndexTimer.current = null;
      if (searchIndex.update(latestSearchLayouts.current)) {
        setSearchRevision((revision) => revision + 1);
      }
    }, SEARCH_INDEX_THROTTLE_MS);
  }, [searchIndex, searchLayouts]);
  useEffect(() => () => {
    if (searchIndexTimer.current !== null) window.clearTimeout(searchIndexTimer.current);
  }, []);

  const searchRecordIds = useMemo(
    () => searchIndex.search(searchQuery),
    [searchIndex, searchQuery, searchRevision],
  );
  const searchMatchIndexes = useMemo(() => {
    if (searchRecordIds === null) return null;
    const indexes = new Set();
    for (const turns of searchLayouts) {
      for (const turn of turns) {
        for (const group of turn.groups) {
          for (const cell of group.cells) {
            if (searchRecordIds.has(trajectoryRecordId(cell))) indexes.add(cell.index);
          }
        }
      }
    }
    return indexes;
  }, [searchLayouts, searchRecordIds]);
  const streamingCells = useMemo(
    () => partialSearchTurns.flatMap((turn) => (
      turn.groups.flatMap((group) => group.cells)
    )),
    [partialSearchTurns],
  );
  const timelineFocusIndexes = useMemo(
    () => timelineSelection === null
      ? null
      : trajectoryTimelineFocusIndexes(timelineTurns, timelineSelection, timelineMode),
    [timelineMode, timelineSelection, timelineTurns],
  );
  const loadEarlierHistory = useCallback(
    () => loadOlderRef.current(),
    [],
  );

  const collapsibleTurnIds = useMemo(
    () => timelineTurns
      .filter((turn) => turn.turn !== null && turn.groups.reduce(
        (count, group) => count + group.cells.filter(
          (cell) => cell.requestOnly !== true && cell.kind !== 'system',
        ).length,
        0,
      ) > 1)
      .map((turn) => turn.turn),
    [timelineTurns],
  );
  const allTurnsCollapsed = collapsibleTurnIds.length > 0
    && collapsibleTurnIds.every((turn) => collapsedTurns.has(turn));
  const collapsibleAssistantIds = useMemo(() => {
    const ids = [];
    for (const turn of timelineTurns) {
      const cells = turn.groups.flatMap((group) => group.cells);
      for (let index = 0; index < cells.length; index += 1) {
        const cell = cells[index];
        const next = cells[index + 1];
        if (cell?.kind === 'message' && (next?.kind === 'tool' || next?.kind === 'subtool')) {
          ids.push(trajectoryRecordId(cell));
        }
      }
    }
    return ids;
  }, [timelineTurns]);
  const allAssistantsCollapsed = collapsibleAssistantIds.length > 0
    && collapsibleAssistantIds.every((id) => collapsedAssistants.has(id));

  const toggleTurn = useCallback((turn) => {
    setCollapsedTurns((current) => {
      const next = new Set(current);
      if (next.has(turn)) next.delete(turn);
      else next.add(turn);
      return next;
    });
  }, []);
  const toggleAllTurns = useCallback(() => {
    setCollapsedTurns((current) => {
      const next = new Set(current);
      for (const turn of collapsibleTurnIds) {
        if (allTurnsCollapsed) next.delete(turn);
        else next.add(turn);
      }
      return next;
    });
  }, [allTurnsCollapsed, collapsibleTurnIds]);
  const toggleAssistant = useCallback((id) => {
    setCollapsedAssistants((current) => {
      const next = new Set(current);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }, []);
  const toggleAllAssistants = useCallback(() => {
    setCollapsedAssistants((current) => {
      const next = new Set(current);
      for (const id of collapsibleAssistantIds) {
        if (allAssistantsCollapsed) next.delete(id);
        else next.add(id);
      }
      return next;
    });
  }, [allAssistantsCollapsed, collapsibleAssistantIds]);

  return (
    <div className={`${css.root} ace-trajectory-deepseek-theme`} data-conversation-composer-overlay="">
      <TrajectoryToolbar
        actualDuration={actualDuration}
        onActualDurationChange={(value) => {
          setActualDuration(value);
          writeDurationPreference(value);
          setTimelineSelection(null);
        }}
        actualTime={actualTime}
        onActualTimeChange={(value) => {
          setActualTime(value);
          setTimelineSelection(null);
        }}
        allTurnsCollapsed={allTurnsCollapsed}
        onToggleAllTurns={toggleAllTurns}
        allAssistantsCollapsed={allAssistantsCollapsed}
        onToggleAllAssistants={toggleAllAssistants}
        searchQuery={searchQuery}
        onSearchQueryChange={setSearchQuery}
      />
      <TrajectoryTimeline
        turns={timelineTurns}
        mode={timelineMode}
        range={timelineSelection}
        hasEarlierRecords={hasOlderRecords}
        onLoadEarlier={loadEarlierHistory}
        selectedIndex={selectedTimelineIndex}
        searchMatchIndexes={searchMatchIndexes}
        onRangeChange={setTimelineSelection}
        onRecordSelect={(index) => {
          setTimelineSelection(null);
          setTimelineRecordSelection({ index });
          setSelectedTimelineIndex(index);
        }}
        onRecordFocus={(index) => setTimelineRecordFocus({ index })}
      />
      <div className={css.ledger} style={{ '--dsh-trajectory-bottom-clearance': '0px' }}>
        <TrajectoryTable
          requestNumbers={projection.requestNumbers}
          turns={timelineTurns}
          streamingCells={streamingCells}
          timelineFocusIndexes={timelineFocusIndexes}
          searchMatchIndexes={searchMatchIndexes}
          onSelectedIndexChange={setSelectedTimelineIndex}
          onRecordSelect={(index) => {
            if (timelineFocusIndexes !== null && !timelineFocusIndexes.has(index)) {
              setTimelineSelection(null);
            }
          }}
          recordSelection={timelineRecordSelection}
          recordFocus={timelineRecordFocus}
          historyLoading={historyLoading}
          olderHistoryLoading={olderHistoryLoading}
          historyStartSeq={projection.historyStartSeq}
          hasOlderRecords={hasOlderRecords}
          onLoadOlder={loadEarlierHistory}
          onClearSelection={() => setTimelineSelection(null)}
          collapsedTurns={collapsedTurns}
          onToggleTurn={toggleTurn}
          collapsedAssistants={collapsedAssistants}
          onToggleAssistant={toggleAssistant}
          inspectCallId={inspectCallId}
          onInspectApplied={onInspectApplied}
        />
      </div>
    </div>
  );
}
