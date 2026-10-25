import { homeRefFromWorkspace, noHomeWorkspaceOption } from './homeWorkspaceSelection.js';

export const HOME_INTENT_REFRESH_MS = 30 * 60 * 1000;
export const HOME_INTENT_CATEGORIES = Object.freeze({
  writing: { icon: 'edit', label: '写作' },
  exploration: { icon: 'searchSparkle', label: '探索' },
  execution: { icon: 'run', label: '执行' },
  thinking: { icon: 'lightbulb', label: '思考' },
});

export const DEFAULT_HOME_INTENT_PROMPTS = Object.freeze([
  { category: 'writing', title: '写一份清晰的文档', description: '把想法整理成可用的初稿',
    prompt: '请帮我起草一份文档。先结合当前上下文梳理目标、读者和关键内容；缺少必要信息时向我提问，再给出结构清晰、可以继续修改的初稿。' },
  { category: 'exploration', title: '探索一个问题', description: '查找线索，理解来龙去脉',
    prompt: '请帮我探索当前项目或问题。先阅读相关资料，梳理关键结构、调用关系和已有结论，用具体证据解释你的发现，并列出最值得继续调查的问题。' },
  { category: 'execution', title: '完成一项任务', description: '落实改动并验证结果',
    prompt: '请帮我完成接下来要做的任务。先确认目标和当前状态，给出简短步骤，然后实施必要的修改并验证结果；保留无关的已有改动。' },
  { category: 'thinking', title: '一起推敲方案', description: '比较选择，找到下一步',
    prompt: '请和我一起推敲一个方案。先明确问题、目标与约束，比较可行选择的收益、代价和风险，指出关键假设，再给出建议与下一步行动。' },
]);

const HOME_INTENT_ERROR_MESSAGES = Object.freeze({
  TIMEOUT: '生成任务建议超时，请稍后按 Ctrl+Y 重试',
  MODEL_UNAVAILABLE: '没有可用的模型，请在「设置 > 工具 > 摘要生成」中选择模型',
  GENERATION_FAILED: '模型请求失败，请检查摘要生成使用的模型后按 Ctrl+Y 重试',
  INVALID_RESPONSE: '模型返回的任务建议无法识别，请按 Ctrl+Y 重试或更换摘要生成模型',
});

// Accepts a snapshot error code or a thrown request error.
export function homeIntentErrorMessage(error) {
  const code = typeof error === 'string' ? error : error?.code;
  if (Object.hasOwn(HOME_INTENT_ERROR_MESSAGES, code || '')) return HOME_INTENT_ERROR_MESSAGES[code];
  if (error?.status === 404) return '后台服务版本过旧，不支持任务建议，请重启 ACECode';
  return '刷新任务建议失败，请按 Ctrl+Y 重试';
}

export function normalizeHomeIntentPrompts(value) {
  if (!Array.isArray(value) || value.length > 4) return [];
  return value.filter((item) => item && Object.hasOwn(HOME_INTENT_CATEGORIES, item.category)
    && typeof item.title === 'string' && item.title.trim()
    && typeof item.description === 'string' && typeof item.prompt === 'string' && item.prompt.trim()
    && item.workspace && typeof item.workspace.noWorkspace === 'boolean'
    && (item.workspace.noWorkspace || (typeof item.workspace.hash === 'string' && item.workspace.hash
      && typeof item.workspace.cwd === 'string' && item.workspace.cwd)))
    .map((item) => ({ ...item, workspace: item.workspace.noWorkspace ? noHomeWorkspaceOption() : item.workspace }));
}

export function homeIntentPromptRef(item, currentWorkspace, currentRef, health) {
  const workspace = item.workspace || currentWorkspace || noHomeWorkspaceOption();
  return { ...homeRefFromWorkspace(workspace, currentRef, health), initialHomePrompt: item.prompt };
}

export function isHomeIntentRefreshShortcut(event) {
  return !!event.ctrlKey && !event.metaKey && !event.altKey && !event.shiftKey
    && !event.isComposing && String(event.key).toLowerCase() === 'y';
}

// App-owned, independent of the active page. Timers and clock are injectable so
// refresh, sleep/resume and concurrency behavior do not require real waits.
export function createHomeIntentController({ api, onChange = () => {}, onError = () => {},
  now = Date.now, setTimer = setTimeout, clearTimer = clearTimeout }) {
  let snapshot = { suggestions: [], refreshing: false, next_refresh_at_ms: 0 };
  let disposed = false;
  let timer;
  let inFlight;
  let inFlightMode;
  let abort;
  let manualPending = false;
  let reportManualResult = false;
  let pollingSince = 0;

  function publish(value) {
    if (disposed) return;
    snapshot = { ...value, suggestions: normalizeHomeIntentPrompts(value?.suggestions) };
    onChange(snapshot);
  }

  function schedule() {
    clearTimer(timer);
    if (disposed) return;
    const delay = snapshot.refreshing ? 1500 : Math.max(0, snapshot.next_refresh_at_ms - now());
    timer = setTimer(() => { void request(snapshot.refreshing ? 'read' : 'refresh'); }, delay);
  }

  function request(kind) {
    if (disposed) return Promise.resolve(snapshot);
    if (kind === 'force') reportManualResult = true;
    if (inFlight) {
      if (kind === 'force' && inFlightMode === 'read' && !snapshot.refreshing) manualPending = true;
      return inFlight;
    }
    clearTimer(timer);
    abort = new AbortController();
    const controller = abort;
    const mode = snapshot.refreshing ? 'read' : (manualPending ? 'force' : kind);
    inFlightMode = mode;
    if (mode === 'force') manualPending = false;
    inFlight = Promise.resolve().then(async () => {
      try {
        const value = mode === 'read'
          ? await api.getHomeIntentPrompts({ signal: controller.signal })
          : await api.refreshHomeIntentPrompts(mode === 'force', { signal: controller.signal });
        if (disposed) return snapshot;
        const next = { ...value, next_refresh_at_ms: Number(value?.next_refresh_at_ms) || now() };
        if (next.refreshing) {
          if (!pollingSince) pollingSince = now();
          // Longer than the server's preparation and model budgets combined.
          if (now() - pollingSince > 120_000) throw Object.assign(new Error('TIMEOUT'), { code: 'TIMEOUT' });
        } else pollingSince = 0;
        publish(next);
        if (!next.refreshing) {
          if (next.error && reportManualResult) onError(next.error);
          reportManualResult = false;
        }
      } catch (error) {
        if (disposed || error?.name === 'AbortError') return snapshot;
        publish({ ...snapshot, refreshing: false, next_refresh_at_ms: now() + HOME_INTENT_REFRESH_MS });
        if (reportManualResult) onError(error);
        reportManualResult = false;
      } finally {
        inFlight = null;
        inFlightMode = null;
        abort = null;
        if (!disposed) {
          if (manualPending && !snapshot.refreshing) { manualPending = false; void request('force'); }
          else schedule();
        }
      }
      return snapshot;
    });
    return inFlight;
  }

  return {
    start: () => request('read'),
    refresh: () => request('force'),
    wake() {
      if (snapshot.refreshing) return request('read');
      if (now() >= snapshot.next_refresh_at_ms) return request('refresh');
      return Promise.resolve(snapshot);
    },
    snapshot: () => snapshot,
    dispose() { disposed = true; clearTimer(timer); abort?.abort(); },
  };
}
