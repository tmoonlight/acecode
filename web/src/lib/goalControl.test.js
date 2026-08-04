import assert from 'node:assert/strict';
import {
  formatGoalElapsedSeconds,
  getGoalStopControlState,
  getGoalTrayState,
  goalElapsedSeconds,
  goalProgressText,
  goalStatusAction,
  goalStatusLabel,
  isActiveGoal,
  normalizeGoal,
  normalizeGoalStatus,
  shouldAbortForStopControl,
} from './goalControl.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('idle active goal uses the goal tray instead of the interrupt control', () => {
  const state = getGoalStopControlState({ goal: { status: 'active' }, busy: false });
  assert.equal(isActiveGoal({ status: 'active' }), true);
  assert.equal(state.visible, false);
  assert.equal(state.action, 'none');
  assert.equal(state.label, '');
  assert.equal(shouldAbortForStopControl({ goal: { status: 'active' }, busy: false }), false);
});

run('busy non-goal turn still shows abort control', () => {
  const state = getGoalStopControlState({ goal: null, busy: true });
  assert.equal(state.visible, true);
  assert.equal(state.action, 'abort');
  assert.equal(state.label, '中断');
});

run('busy active goal makes the stop action an abort', () => {
  const state = getGoalStopControlState({ goal: { status: 'active' }, busy: true });
  assert.equal(state.visible, true);
  assert.equal(state.action, 'abort');
  assert.equal(state.label, '中断');
  assert.equal(state.title, '中断当前任务');
  assert.equal(shouldAbortForStopControl({ goal: { status: 'active' }, busy: true }), true);
});

run('paused goal without a running turn hides stop control', () => {
  const state = getGoalStopControlState({ goal: { status: 'paused' }, busy: false });
  assert.equal(isActiveGoal({ status: 'paused' }), false);
  assert.equal(state.visible, false);
  assert.equal(state.action, 'none');
});

run('blocked goal without a running turn hides stop control', () => {
  const state = getGoalStopControlState({ goal: { status: 'blocked' }, busy: false });
  assert.equal(isActiveGoal({ status: 'blocked' }), false);
  assert.equal(state.visible, false);
  assert.equal(state.action, 'none');
});

run('goal statuses normalize camel case, snake case, and dashed spellings', () => {
  assert.equal(normalizeGoalStatus('active'), 'active');
  assert.equal(normalizeGoalStatus('usage_limited'), 'usage_limited');
  assert.equal(normalizeGoalStatus('usageLimited'), 'usage_limited');
  assert.equal(normalizeGoalStatus('budget-limited'), 'budget_limited');
  assert.equal(normalizeGoalStatus('unknown'), '');
});

run('goal payload normalization accepts snake and camel field names', () => {
  const snake = normalizeGoal({
    thread_id: 's1',
    goal_id: 'g1',
    objective: 'ship it',
    status: 'usage_limited',
    token_budget: 50000,
    tokens_used: 12500,
    time_used_seconds: 61,
    created_at_ms: 1000,
    updated_at_ms: 2000,
  });
  assert.deepEqual(
    {
      threadId: snake.threadId,
      goalId: snake.goalId,
      objective: snake.objective,
      status: snake.status,
      tokenBudget: snake.tokenBudget,
      tokensUsed: snake.tokensUsed,
      timeUsedSeconds: snake.timeUsedSeconds,
      createdAtMs: snake.createdAtMs,
      updatedAtMs: snake.updatedAtMs,
    },
    {
      threadId: 's1',
      goalId: 'g1',
      objective: 'ship it',
      status: 'usage_limited',
      tokenBudget: 50000,
      tokensUsed: 12500,
      timeUsedSeconds: 61,
      createdAtMs: 1000,
      updatedAtMs: 2000,
    },
  );

  const camel = normalizeGoal({
    threadId: 's2',
    goalId: 'g2',
    objective: 'verify it',
    status: 'budgetLimited',
    tokenBudget: 80000,
    tokensUsed: 80100,
    timeUsedSeconds: 90,
    createdAtMs: 3000,
    updatedAtMs: 4000,
  });
  assert.equal(camel.threadId, 's2');
  assert.equal(camel.status, 'budget_limited');
  assert.equal(camel.tokenBudget, 80000);
  assert.equal(camel.updatedAtMs, 4000);
});

run('every persisted goal status has the Codex-aligned label and action', () => {
  const cases = [
    ['active', '进行中的目标', 'pause'],
    ['paused', '已暂停的目标', 'resume'],
    ['blocked', '目标受阻', 'resume'],
    ['usage_limited', '目标使用受限', 'resume'],
    ['budget_limited', '目标受限', null],
    ['complete', '已达成目标', null],
  ];
  for (const [status, label, action] of cases) {
    assert.equal(goalStatusLabel(status), label);
    assert.equal(goalStatusAction(status), action);
    const tray = getGoalTrayState({ status, objective: 'finish work' }, 0);
    assert.equal(tray.visible, true);
    assert.equal(tray.label, label);
    assert.equal(tray.statusAction, action);
  }
  assert.equal(getGoalTrayState(null).visible, false);
});

run('elapsed formatting stays compact from seconds through days', () => {
  assert.equal(formatGoalElapsedSeconds(0), '0s');
  assert.equal(formatGoalElapsedSeconds(59), '59s');
  assert.equal(formatGoalElapsedSeconds(60), '1m');
  assert.equal(formatGoalElapsedSeconds(61), '1m 1s');
  assert.equal(formatGoalElapsedSeconds(3661), '1h 1m 1s');
  assert.equal(formatGoalElapsedSeconds(183845), '2d 3h 4m 5s');
});

run('active elapsed time advances from updated timestamp and clamps clock rollback', () => {
  const active = {
    status: 'active',
    time_used_seconds: 8,
    updated_at_ms: 100000,
  };
  assert.equal(goalElapsedSeconds(active, 105999), 13);
  assert.equal(goalElapsedSeconds(active, 99000), 8);
  assert.equal(goalElapsedSeconds({ ...active, status: 'paused' }, 200000), 8);
});

run('active and budget-limited goals prefer K/M token progress', () => {
  assert.equal(goalProgressText({
    status: 'active',
    token_budget: 80000,
    tokens_used: 12500,
    time_used_seconds: 60,
  }, 0), '12.5K / 80K');
  assert.equal(goalProgressText({
    status: 'budgetLimited',
    tokenBudget: 2000000,
    tokensUsed: 1250000,
    timeUsedSeconds: 60,
  }, 0), '1.25M / 2M');
  assert.equal(goalProgressText({
    status: 'paused',
    token_budget: 80000,
    tokens_used: 12500,
    time_used_seconds: 60,
  }, 0), '1m');
});

run('unbudgeted active goal progress uses live elapsed time', () => {
  assert.equal(goalProgressText({
    status: 'active',
    token_budget: null,
    time_used_seconds: 8,
    updated_at_ms: 100000,
  }, 103999), '11s');
});
