import { officeSessionRef, projectDesktopOffice } from './desktopOfficeState.js';

// Streaming text reaches the office a few times per second, not per token.
export const OFFICE_LIVE_PUBLISH_MS = 150;
const LIVE_TEXT_CHARS = 400;

// One App-owned controller; every asynchronous response belongs to a selection
// generation. Inject IO, timers and publishing so lifecycle races are testable.
export function createDesktopOfficeController({
  fetchSnapshot, fetchModel, publish, retainSession = () => {}, releaseSession = () => {},
  schedule = setTimeout, cancel = clearTimeout, initial = {},
} = {}) {
  let follow = initial.follow !== false, active = {}, selected = initial.selected || {};
  let generation = 0, disposed = false, timer = null, timerDelay = Infinity, request = null;
  let snapshot = {}, connected = true, queued = false, liveTimer = null;
  let modelRequest = null, modelRequestKey = '', modelContext = null;
  const retained = new Set();
  // session id -> {phase, seq, fromSeq, text}: token/reasoning events newer than the snapshot.
  const live = new Map();
  const project = () => projectDesktopOffice(snapshot, {follow, connected, live});
  const emit = () => !disposed && publish(project());
  const emitLive = () => {
    if (disposed || liveTimer !== null) return;
    liveTimer = schedule(() => {liveTimer = null; emit();}, OFFICE_LIVE_PUBLISH_MS);
  };
  // Streamed text reaches both office members and companion-pet session cards.
  const liveSessions = () => [...(snapshot.agents || []), ...(snapshot.activities || [])];
  const tracked = id => liveSessions().some(session => session.id === id);
  const pruneLive = () => {
    for (const [id, entry] of live) {
      const session = liveSessions().find(item => item.id === id);
      if (!session || entry.seq <= (Number(session.activity?.seq) || 0)) live.delete(id);
    }
  };
  const syncRetained = () => {
    const wanted = new Set();
    for (const session of [...(snapshot.offices || []), ...(snapshot.agents || []), ...(snapshot.activities || [])]) {
      if (session.active && (session.id === snapshot.selected?.id || session.busy || session.status === 'running')) wanted.add(session.id);
    }
    for (const id of retained) if (!wanted.has(id)) {releaseSession(id); retained.delete(id);}
    for (const id of wanted) if (!retained.has(id)) {retainSession(id); retained.add(id);}
  };
  const modelKey = session => JSON.stringify([session?.id, session?.workspace_hash || '']);
  const cancelModel = () => {
    modelRequest?.abort(); modelRequest = null; modelRequestKey = ''; modelContext = null;
  };
  const applyModelWindow = limit => {
    const id = snapshot.selected.id;
    snapshot = {...snapshot, selected: {...snapshot.selected, context_window: limit},
      agents: (snapshot.agents || []).map(agent => agent.id === id ? {...agent, context_window: limit} : agent)};
  };
  async function fillModelWindow() {
    if (!snapshot.selected || snapshot.selected.context_window || !fetchModel || modelRequest) return;
    const selected = snapshot.selected, key = modelKey(selected), version = generation;
    const controller = new AbortController();
    modelRequest = controller; modelRequestKey = key;
    try {
      const model = await fetchModel(selected.id, selected.workspace_hash || '', {signal: controller.signal});
      if (disposed || version !== generation || controller.signal.aborted || modelKey(snapshot.selected) !== key) return;
      const limit = Number(model?.context_window ?? model?.contextWindow) || 0;
      if (!Number.isFinite(limit) || limit <= 0 || snapshot.selected.context_window) return;
      modelContext = {key, limit};
      // Enrich the current snapshot; never replay the older room/activity that
      // originally triggered this optional query.
      applyModelWindow(limit); emit();
    } catch { /* Unknown context must not block the room or mark it offline. */ }
    finally {if (modelRequest === controller) modelRequest = null;}
  }
  function later(delay) {
    if (disposed) return;
    if (timer !== null && timerDelay <= delay) return;
    if (timer !== null) cancel(timer);
    timerDelay = delay;
    timer = schedule(() => {timer = null; timerDelay = Infinity; void refresh();}, delay);
  }
  async function refresh() {
    if (disposed) return;
    if (request) {queued = true; return;}
    const version = generation, controller = new AbortController();
    request = controller;
    try {
      const wanted = follow && active.sessionId ? active : selected;
      const next = await fetchSnapshot(wanted, {signal: controller.signal});
      if (disposed || version !== generation || controller.signal.aborted) return;
      snapshot = next; connected = true;
      if (!next.selected || next.selected.context_window ||
          (modelRequest && modelRequestKey !== modelKey(next.selected)) ||
          (modelContext && modelContext.key !== modelKey(next.selected))) cancelModel();
      if (next.selected && !next.selected.context_window && modelContext) applyModelWindow(modelContext.limit);
      if (next.selected) selected = officeSessionRef(next.selected);
      pruneLive(); syncRetained(); emit();
      // Paper-stack sizing is optional: display the target room immediately.
      void fillModelWindow();
    } catch (error) {
      if (disposed || version !== generation || controller.signal.aborted) return;
      if (error?.status === 404) {
        cancelModel();
        selected = {}; snapshot = {...snapshot, selected:null, agents:[], offices:error.body?.offices || snapshot.offices || [],
          activities:error.body?.activities || snapshot.activities || []};
        if (follow) active = {};
        live.clear(); syncRetained();
      } else connected = false;
      emit();
    } finally {
      if (request === controller) request = null;
      if (!disposed && version === generation) {
        const delay = queued ? 100 : snapshot.complete === false ? 500 : 3000;
        queued = false; later(delay);
      }
    }
  }
  function change() {
    generation++;
    request?.abort(); request = null; queued = false;
    cancelModel();
    // Keep the last coherent room until the new snapshot is ready. Publishing
    // an empty selection here rebuilds the default layout on every switch.
    emit(); later(0);
  }
  function streamed(message) {
    const id = String(message.session_id || '');
    if (!id || !tracked(id)) return;
    const phase = message.type === 'token' ? 'responding' : 'reasoning';
    const seq = Number(message.seq) || 0, piece = String(message.payload?.text || '');
    const prev = live.get(id);
    live.set(id, prev && prev.phase === phase
      ? {...prev, seq: Math.max(seq, prev.seq), text: (prev.text + piece).slice(-LIVE_TEXT_CHARS)}
      : {phase, seq, fromSeq: seq, text: piece.slice(-LIVE_TEXT_CHARS)});
    emitLive();
  }
  return {
    start() {later(0);},
    refresh,
    setActive(value) {
      const next = officeSessionRef(value);
      if (next.sessionId === active.sessionId && next.workspaceHash === active.workspaceHash) return;
      active = next;
      if (follow && next.sessionId) change();
    },
    select(value) {selected = officeSessionRef(value); follow = false; change();},
    setFollow(value) {follow = !!value; change();},
    onEvent(message) {
      if (message.type === 'token' || message.type === 'reasoning') {streamed(message); return;}
      if (message.type === 'tool_update') return;
      if (request) queued = true;
      else later(100);
    },
    disconnected() {connected = false; emit(); later(500);},
    reconnected() {
      generation++; request?.abort(); request = null; queued = false;
      cancelModel();
      // Preserve retained subscriptions while recovering: releasing them from
      // an onopen callback would close/reopen an office-only connection forever.
      later(0);
    },
    get state() {return project();},
    dispose() {
      disposed = true; generation++; request?.abort(); request = null;
      cancelModel();
      if (timer !== null) cancel(timer);
      if (liveTimer !== null) cancel(liveTimer);
      timer = null; liveTimer = null; live.clear();
      for (const id of retained) releaseSession(id);
      retained.clear();
    },
  };
}
