import { formatUsageTokens } from './usageStats.js';

const GOAL_STATUSES = new Set([
  'active',
  'paused',
  'blocked',
  'usage_limited',
  'budget_limited',
  'complete',
]);

const GOAL_STATUS_LABELS = Object.freeze({
  active: '进行中的目标',
  paused: '已暂停的目标',
  blocked: '目标受阻',
  usage_limited: '目标使用受限',
  budget_limited: '目标受限',
  complete: '已达成目标',
});

function nonNegativeInteger(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return 0;
  return Math.max(0, Math.trunc(number));
}

function optionalNonNegativeInteger(value) {
  if (value == null || value === '') return null;
  const number = Number(value);
  if (!Number.isFinite(number)) return null;
  return Math.max(0, Math.trunc(number));
}

function readGoalField(goal, snakeKey, camelKey) {
  return goal?.[snakeKey] ?? goal?.[camelKey];
}

export function normalizeGoalStatus(status) {
  const normalized = String(status || '')
    .replace(/([a-z0-9])([A-Z])/g, '$1_$2')
    .replace(/[\s-]+/g, '_')
    .toLowerCase();
  return GOAL_STATUSES.has(normalized) ? normalized : '';
}

export function normalizeGoal(goal) {
  if (!goal || typeof goal !== 'object') return null;
  return {
    ...goal,
    threadId: String(readGoalField(goal, 'thread_id', 'threadId') || ''),
    goalId: String(readGoalField(goal, 'goal_id', 'goalId') || ''),
    objective: String(goal.objective || ''),
    status: normalizeGoalStatus(goal.status),
    tokenBudget: optionalNonNegativeInteger(readGoalField(goal, 'token_budget', 'tokenBudget')),
    tokensUsed: nonNegativeInteger(readGoalField(goal, 'tokens_used', 'tokensUsed')),
    timeUsedSeconds: nonNegativeInteger(readGoalField(goal, 'time_used_seconds', 'timeUsedSeconds')),
    createdAtMs: nonNegativeInteger(readGoalField(goal, 'created_at_ms', 'createdAtMs')),
    updatedAtMs: nonNegativeInteger(readGoalField(goal, 'updated_at_ms', 'updatedAtMs')),
  };
}

export function goalStatusLabel(status) {
  return GOAL_STATUS_LABELS[normalizeGoalStatus(status)] || '目标';
}

export function goalStatusAction(status) {
  switch (normalizeGoalStatus(status)) {
    case 'active':
      return 'pause';
    case 'paused':
    case 'blocked':
    case 'usage_limited':
      return 'resume';
    default:
      return null;
  }
}

export function formatGoalElapsedSeconds(value) {
  const totalSeconds = nonNegativeInteger(value);
  if (totalSeconds < 60) return `${totalSeconds}s`;

  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor(totalSeconds / 3600) % 24;
  const minutes = Math.floor(totalSeconds / 60) % 60;
  const seconds = totalSeconds % 60;
  if (days > 0 || hours > 0) {
    return [
      days > 0 ? `${days}d` : '',
      hours > 0 ? `${hours}h` : '',
      minutes > 0 ? `${minutes}m` : '',
      seconds > 0 ? `${seconds}s` : '',
    ].filter(Boolean).join(' ');
  }
  return seconds > 0 ? `${minutes}m ${seconds}s` : `${minutes}m`;
}

export function goalElapsedSeconds(goal, nowMs = Date.now()) {
  const normalized = normalizeGoal(goal);
  if (!normalized) return 0;
  if (normalized.status !== 'active' || normalized.updatedAtMs <= 0) {
    return normalized.timeUsedSeconds;
  }
  const currentMs = Number(nowMs);
  const elapsedSinceUpdate = Number.isFinite(currentMs)
    ? Math.max(0, Math.floor((currentMs - normalized.updatedAtMs) / 1000))
    : 0;
  return normalized.timeUsedSeconds + elapsedSinceUpdate;
}

export function goalProgressText(goal, nowMs = Date.now()) {
  const normalized = normalizeGoal(goal);
  if (!normalized) return '';
  const showsTokenProgress = normalized.tokenBudget != null
    && (normalized.status === 'active' || normalized.status === 'budget_limited');
  if (showsTokenProgress) {
    return `${formatUsageTokens(normalized.tokensUsed)} / ${formatUsageTokens(normalized.tokenBudget)}`;
  }
  return formatGoalElapsedSeconds(goalElapsedSeconds(normalized, nowMs));
}

export function getGoalTrayState(goal, nowMs = Date.now()) {
  const normalized = normalizeGoal(goal);
  if (!normalized) {
    return {
      visible: false,
      goal: null,
      label: '',
      progress: '',
      statusAction: null,
      statusActionLabel: '',
    };
  }
  const statusAction = goalStatusAction(normalized.status);
  return {
    visible: true,
    goal: normalized,
    label: goalStatusLabel(normalized.status),
    progress: goalProgressText(normalized, nowMs),
    statusAction,
    statusActionLabel: statusAction === 'pause' ? '暂停目标' : statusAction === 'resume' ? '恢复目标' : '',
  };
}

export function isActiveGoal(goal) {
  return normalizeGoalStatus(goal?.status) === 'active';
}

export function getGoalStopControlState({ busy = false, stopping = false } = {}) {
  const isBusy = !!busy;
  return {
    visible: isBusy,
    action: isBusy ? 'abort' : 'none',
    disabled: !!stopping,
    label: isBusy ? '中断' : '',
    title: isBusy ? '中断当前任务' : '',
  };
}

export function shouldAbortForStopControl(options = {}) {
  return getGoalStopControlState(options).action === 'abort';
}
