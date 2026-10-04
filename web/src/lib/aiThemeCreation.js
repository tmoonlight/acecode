import { isAiColorTheme } from './colorTheme.js';
import { homeRefFromWorkspace } from './homeWorkspaceSelection.js';

export function aiThemeCreationDraft() {
  return '/ai-theme 我想生成关于 XXX 的主题，X 色是它的主色调。';
}

export function aiThemeCreationRef(current, health) {
  return {
    ...homeRefFromWorkspace(current, current, health),
    composerDraftScope: 'ai-theme',
    initialDraftText: aiThemeCreationDraft(),
  };
}

export function homeComposerScopedWorkspace(workspaceHash = '', scope = '') {
  if (scope === 'scheduled-task') return `__scheduled_task__:${workspaceHash}`;
  return scope === 'ai-theme' ? `__ai_theme__:${workspaceHash}` : workspaceHash;
}

// Only a tool invocation observed live in the visible task can change the UI.
// REST history never enters this monitor. Delivery provenance comes from the
// server, not wall-clock comparisons between a remote daemon and the browser.
export function createLiveThemeCreationMonitor({ onCreated, onStart = () => undefined }) {
  let sessionId = '';
  const starts = new Map(), applied = new Set();
  return {
    setSession(next) {
      if (sessionId === next) return;
      sessionId = next || '';
      this.restart();
    },
    restart() { starts.clear(); },
    accept(event) {
      const payload = event?.payload || {};
      if (!sessionId || (event?.session_id || payload.session_id) !== sessionId
          || event.replayed !== false || payload.tool !== 'theme_create') return false;
      const callId = payload.tool_call_id;
      if (typeof callId !== 'string' || !callId) return false;
      const key = `${sessionId}:${callId}`;
      if (applied.has(key)) return false;
      if (event.type === 'tool_start') {
        if (payload.args?.action === 'install' && !starts.has(callId)) starts.set(callId, onStart());
        return false;
      }
      if (event.type !== 'tool_end' || !starts.has(callId)) return false;
      const intent = starts.get(callId);
      starts.delete(callId);
      if (payload.success !== true) return false;
      const theme = payload.metadata?.theme_created;
      if (!isAiColorTheme(theme?.id) || theme.apply !== true || typeof theme.version !== 'string' || !theme.version) return false;
      applied.add(key);
      if (applied.size > 256) applied.delete(applied.values().next().value);
      onCreated(theme, intent);
      return true;
    },
  };
}
