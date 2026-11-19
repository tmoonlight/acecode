import assert from 'node:assert/strict';
import {
  SESSION_START_MASK_DELAY_MS,
  SESSION_START_MASK_MIN_VISIBLE_MS,
  createSlowPendingGate,
  sessionStartMaskApplies,
  sessionStartMaskKind,
} from './sessionStartMask.js';

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

// 手动推进的时钟:定时器按到期时间依次触发,触发时 now() 等于它的到期时间。
function fakeClock() {
  let time = 0;
  let nextId = 1;
  const timers = new Map();
  return {
    now: () => time,
    setTimer: (fn, ms) => {
      const id = nextId++;
      timers.set(id, { at: time + ms, fn });
      return id;
    },
    clearTimer: (id) => { timers.delete(id); },
    advance(ms) {
      const target = time + ms;
      for (;;) {
        let due = null;
        for (const [id, timer] of timers) {
          if (timer.at <= target && (!due || timer.at < due.at)) due = { id, ...timer };
        }
        if (!due) break;
        timers.delete(due.id);
        time = due.at;
        due.fn();
      }
      time = target;
    },
    pendingTimers: () => timers.size,
  };
}

function gateWithClock() {
  const clock = fakeClock();
  const changes = [];
  const gate = createSlowPendingGate({
    onChange: (visible) => changes.push([clock.now(), visible]),
    setTimer: clock.setTimer,
    clearTimer: clock.clearTimer,
    now: clock.now,
  });
  return { clock, changes, gate };
}

// 两个阈值的取值依据:正常建会话几十毫秒,600 毫秒内完成的一律不该看到遮罩;
// 出现后至少停 400 毫秒,否则 610 毫秒完成的那次会闪一帧模糊。
run('阈值:等待 600 毫秒才出现,出现后至少停留 400 毫秒', () => {
  assert.equal(SESSION_START_MASK_DELAY_MS, 600);
  assert.equal(SESSION_START_MASK_MIN_VISIBLE_MS, 400);
});

// 触发场景:普通新建会话,300 毫秒就完成。
// 期望:遮罩从头到尾不出现,也不留下定时器。
run('很快完成的启动不出现遮罩', () => {
  const { clock, changes, gate } = gateWithClock();
  gate.update(true);
  clock.advance(300);
  gate.update(false);
  clock.advance(5000);
  assert.deepEqual(changes, []);
  assert.equal(gate.isVisible(), false);
  assert.equal(clock.pendingTimers(), 0);
});

// 触发场景:大仓库建 worktree 用了 5 秒。
// 期望:第 600 毫秒出现;完成时已经显示了很久,立刻消失。
run('迟迟没完成才出现,完成后立刻消失', () => {
  const { clock, changes, gate } = gateWithClock();
  gate.update(true);
  clock.advance(599);
  assert.equal(gate.isVisible(), false, '阈值之前不出现');
  clock.advance(1);
  assert.equal(gate.isVisible(), true);
  clock.advance(4400);
  gate.update(false);
  assert.deepEqual(changes, [[600, true], [5000, false]]);
  assert.equal(clock.pendingTimers(), 0);
});

// 触发场景:启动在第 700 毫秒完成,遮罩才出现了 100 毫秒。
// 期望:停留到第 1000 毫秒(出现满 400 毫秒)再消失,不闪。
run('刚出现就完成时补足最短停留', () => {
  const { clock, changes, gate } = gateWithClock();
  gate.update(true);
  clock.advance(700);
  gate.update(false);
  assert.equal(gate.isVisible(), true, '还没停够');
  clock.advance(299);
  assert.equal(gate.isVisible(), true);
  clock.advance(1);
  assert.deepEqual(changes, [[600, true], [1000, false]]);
});

// 触发场景:首页建好会话后进入新会话接着建 worktree,期间反复报告「还在等待」。
// 期望:计时从发送那一刻算起,不因为中途再次报告而重新开始。
run('等待期间重复报告不重新计时', () => {
  const { clock, changes, gate } = gateWithClock();
  gate.update(true);
  clock.advance(400);
  gate.update(true);
  clock.advance(200);
  assert.deepEqual(changes, [[600, true]]);
});

// 触发场景:上一次启动的遮罩还在收尾停留,用户又发起了一次。
// 期望:遮罩保持可见,不先灭再亮;第二次结束后按规则消失。
run('收尾停留期间再次开始等待时保持可见', () => {
  const { clock, changes, gate } = gateWithClock();
  gate.update(true);
  clock.advance(700);
  gate.update(false);
  clock.advance(100);
  gate.update(true);
  clock.advance(2000);
  assert.deepEqual(changes, [[600, true]]);
  gate.update(false);
  assert.deepEqual(changes, [[600, true], [2800, false]]);
});

// 触发场景:组件卸载时还有定时器在等。
// 期望:定时器被清掉,之后不再回调。
run('销毁后不再回调', () => {
  const { clock, changes, gate } = gateWithClock();
  gate.update(true);
  gate.dispose();
  clock.advance(5000);
  assert.deepEqual(changes, []);
  assert.equal(clock.pendingTimers(), 0);
});

const homeStart = { token: {}, home: true, worktree: true, sessionId: '' };
const promoted = { ...homeStart, sessionId: 'new-session' };

// 触发场景:从首页发起,会话还没建好 / 建好后界面切进新会话。
// 期望:首页和新会话都算这次启动的界面;中间即使有一帧界面还停在首页也不掉。
run('首页发起的启动盖住首页,会话建好后跟进新会话', () => {
  assert.equal(sessionStartMaskApplies(homeStart, { sessionId: '' }), true);
  assert.equal(sessionStartMaskApplies(promoted, { sessionId: '' }), true);
  assert.equal(sessionStartMaskApplies(promoted, { sessionId: 'new-session', arrived: true }), true);
});

// 触发场景:等待中用户点了别的会话、定时任务页,或进过新会话后又点「新建任务」回到首页。
// 期望:这些界面都不被挡住。回归:只按「没有会话 id」判断时,遮罩会跟到下一个新建任务页上。
run('等待中切到别处不被挡住', () => {
  assert.equal(sessionStartMaskApplies(promoted, { sessionId: 'another-session' }), false);
  assert.equal(sessionStartMaskApplies(homeStart, { sessionId: '', auxiliaryPage: true }), false);
  assert.equal(sessionStartMaskApplies(promoted, { sessionId: '', arrived: true }), false);
  assert.equal(sessionStartMaskApplies(null, { sessionId: '' }), false);
});

// 触发场景:在已有的空会话里发首条消息并勾了 worktree。
// 期望:只盖这个会话;回到首页不算。
run('已有空会话里发起的启动只盖该会话', () => {
  const marker = { token: {}, home: false, worktree: true, sessionId: 'empty-session' };
  assert.equal(sessionStartMaskApplies(marker, { sessionId: 'empty-session' }), true);
  assert.equal(sessionStartMaskApplies(marker, { sessionId: '' }), false);
  assert.equal(sessionStartMaskApplies(marker, { sessionId: 'another-session' }), false);
});

// 触发场景:遮罩上的那一行字。
// 期望:会话建好之前是「新建会话」;建好之后还在等的只剩 worktree。
run('文案:会话建好前是新建会话,之后是建 worktree', () => {
  assert.equal(sessionStartMaskKind(null), '');
  assert.equal(sessionStartMaskKind(homeStart), 'session');
  assert.equal(sessionStartMaskKind(promoted), 'worktree');
  assert.equal(sessionStartMaskKind({ ...promoted, worktree: false }), 'session');
});
