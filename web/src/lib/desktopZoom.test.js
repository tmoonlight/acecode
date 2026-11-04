// 桌面版 Ctrl+滚轮页面缩放:档位换算、滚轮累积、浮层显隐与 native 桥接的回报处理。

import assert from 'node:assert/strict';
import {
  DESKTOP_ZOOM_HIDE_DELAY_MS,
  createDesktopZoomController,
  desktopZoomPercent,
  isDefaultDesktopZoom,
  nextWheelZoom,
  stepDesktopZoom,
} from './desktopZoom.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const flush = () => new Promise((resolve) => setTimeout(resolve, 0));

// 假计时器:只记录最近一次挂上的隐藏回调,测试里手动触发。
function fakeTimers() {
  let nextId = 1;
  const pending = new Map();
  return {
    setTimer: (fn) => {
      const id = nextId++;
      pending.set(id, fn);
      return id;
    },
    clearTimer: (id) => pending.delete(id),
    fireAll: () => {
      const callbacks = [...pending.values()];
      pending.clear();
      for (const fn of callbacks) fn();
    },
    count: () => pending.size,
  };
}

// 假 native 桥接:[] 查询、[factor] 设置,回报 native 实际采用的值。
function fakeBridge({ factor = 1, ok = true } = {}) {
  const calls = [];
  const bridge = (...args) => {
    calls.push(args);
    if (args.length) factor = args[0];
    return Promise.resolve({ ok, factor });
  };
  return { bridge, calls };
}

const notch = (up) => ({ deltaY: up ? -100 : 100, wheelDeltaY: up ? 120 : -120, deltaMode: 0 });

// 场景:从 100% 逐档放大 / 缩小,以及停在档位之间的值(上次保存的比例不在档位上)。
// 期望:每次走到相邻档位;两端到头不再变化。
await run('zoom steps walk the fixed levels and stop at both ends', () => {
  assert.equal(stepDesktopZoom(1, 1), 1.1);
  assert.equal(stepDesktopZoom(1, -1), 0.9);
  assert.equal(stepDesktopZoom(1.2, 1), 1.25);
  assert.equal(stepDesktopZoom(1.2, -1), 1.1);
  assert.equal(stepDesktopZoom(3, 1), 3);
  assert.equal(stepDesktopZoom(0.5, -1), 0.5);
  assert.equal(stepDesktopZoom(Number.NaN, 1), 1.1);
});

// 场景:浮层上显示的比例文字。
// 期望:四舍五入成整数百分比,0.67 显示成 67% 而不是 67.00000001%。
await run('zoom percent label rounds to whole percents', () => {
  assert.equal(desktopZoomPercent(1), '100%');
  assert.equal(desktopZoomPercent(0.67), '67%');
  assert.equal(desktopZoomPercent(1.25), '125%');
  assert.equal(isDefaultDesktopZoom(1.0000001), true);
  assert.equal(isDefaultDesktopZoom(1.1), false);
});

// 场景:普通鼠标滚轮一格(wheelDeltaY = ±120)。
// 期望:每格立即换一档,向上放大、向下缩小;放大后 deltaY 变小也不影响(按格数判断)。
await run('mouse wheel notches step once per notch', () => {
  assert.equal(nextWheelZoom(null, notch(true), 0).direction, 1);
  assert.equal(nextWheelZoom(null, notch(false), 0).direction, -1);
  assert.equal(nextWheelZoom(null, { deltaY: -33, wheelDeltaY: 120, deltaMode: 0 }, 0).direction, 1);
  assert.equal(nextWheelZoom(null, { deltaY: 3, deltaMode: 1 }, 0).direction, -1);
  assert.equal(nextWheelZoom(null, { deltaY: 0 }, 0).direction, 0);
});

// 场景:触控板双指捏合 / 高精度滚轮,每次只给几个像素的增量。
// 期望:同方向连续累积够一格才换一档;停顿超过 300ms 或方向反过来就重新累积,不会轻轻一推连跳几档。
await run('small pixel deltas accumulate before stepping', () => {
  const small = (deltaY) => ({ deltaY, wheelDeltaY: -deltaY * 1.2, deltaMode: 0 });
  let state = nextWheelZoom(null, small(-20), 0);
  assert.equal(state.direction, 0);
  state = nextWheelZoom(state.accumulator, small(-20), 40);
  assert.equal(state.direction, 0);
  state = nextWheelZoom(state.accumulator, small(-20), 80);
  assert.equal(state.direction, 1);

  const paused = nextWheelZoom(nextWheelZoom(null, small(-40), 0).accumulator, small(-40), 500);
  assert.equal(paused.direction, 0);
  const reversed = nextWheelZoom(nextWheelZoom(null, small(-40), 0).accumulator, small(40), 10);
  assert.equal(reversed.direction, 0);
});

// 场景:启动后读 native 当前比例(上次退出保存的 125%),然后滚轮放大一格。
// 期望:读回的比例作为起点,放大到 150% 并把 1.5 发给 native;浮层立即出现,1.5 秒后自动隐藏。
await run('wheel zoom starts from the native factor and shows the indicator', async () => {
  const timers = fakeTimers();
  const { bridge, calls } = fakeBridge({ factor: 1.25 });
  const controller = createDesktopZoomController({ bridge, ...timers });
  await controller.load();
  assert.deepEqual(controller.getSnapshot(), { factor: 1.25, visible: false, available: true });

  controller.handleWheel(notch(true));
  assert.equal(controller.getSnapshot().factor, 1.5);
  assert.equal(controller.getSnapshot().visible, true);
  await flush();
  assert.deepEqual(calls, [[], [1.5]]);

  timers.fireAll();
  assert.equal(controller.getSnapshot().visible, false);
});

// 场景:鼠标停在浮层上准备点「重置」。
// 期望:悬停期间不自动隐藏;点重置把 1 发给 native 并显示 100%,重置按钮随之变成不可点;移开后照常隐藏。
await run('hovering keeps the indicator open and reset returns to 100%', async () => {
  const timers = fakeTimers();
  const { bridge, calls } = fakeBridge({ factor: 1.75 });
  const controller = createDesktopZoomController({ bridge, ...timers });
  await controller.load();
  controller.handleWheel(notch(false));
  controller.setPinned(true);
  assert.equal(timers.count(), 0);

  controller.reset();
  await flush();
  assert.equal(controller.getSnapshot().factor, 1);
  assert.deepEqual(calls.at(-1), [1]);
  assert.equal(timers.count(), 0);

  controller.setPinned(false);
  assert.equal(timers.count(), 1);
  timers.fireAll();
  assert.equal(controller.getSnapshot().visible, false);
});

// 场景:已经到 300% 还继续往上滚。
// 期望:不再发请求,但浮层照样弹出,让用户看到已经到头。
await run('wheel at the limit only re-shows the indicator', async () => {
  const timers = fakeTimers();
  const { bridge, calls } = fakeBridge({ factor: 3 });
  const controller = createDesktopZoomController({ bridge, ...timers });
  await controller.load();
  controller.handleWheel(notch(true));
  await flush();
  assert.deepEqual(calls, [[]]);
  assert.equal(controller.getSnapshot().visible, true);
});

// 场景:快速连滚两格,第一格的回报晚于第二格到达。
// 期望:只认最后一次请求,浮层停在第二格的比例,不被迟到的第一格回报拉回去。
await run('out of order bridge replies keep the latest request', async () => {
  const resolvers = [];
  const bridge = (...args) => new Promise((resolve) => {
    resolvers.push(() => resolve({ ok: true, factor: args.length ? args[0] : 1 }));
  });
  const controller = createDesktopZoomController({ bridge, ...fakeTimers() });
  const loading = controller.load();
  resolvers.shift()();
  await loading;

  controller.handleWheel(notch(true));
  controller.handleWheel(notch(true));
  await flush();
  assert.equal(controller.getSnapshot().factor, 1.25);
  resolvers[1]();
  await flush();
  resolvers[0]();
  await flush();
  assert.equal(controller.getSnapshot().factor, 1.25);
});

// 场景:浏览器直连(没有 aceDesktop_zoomFactor),或平台不支持(native 回 ok:false)。
// 期望:不显示浮层、不缩放;不支持时第一次查询就判定不可用,之后的滚轮什么也不做。
await run('missing or unsupported bridge disables page zoom', async () => {
  const browser = createDesktopZoomController({ bridge: null, ...fakeTimers() });
  await browser.load();
  browser.handleWheel(notch(true));
  assert.deepEqual(browser.getSnapshot(), { factor: 1, visible: false, available: false });

  const { bridge, calls } = fakeBridge({ ok: false });
  const unsupported = createDesktopZoomController({ bridge, ...fakeTimers() });
  await unsupported.load();
  unsupported.handleWheel(notch(true));
  await flush();
  assert.equal(unsupported.getSnapshot().available, false);
  assert.equal(unsupported.getSnapshot().visible, false);
  assert.deepEqual(calls, [[]]);
});

// 场景:native 设置失败(例如 WebView 控制器已经没了)。
// 期望:回退到上一次确认过的比例并收起浮层,不让界面显示一个实际没生效的比例。
await run('failed bridge update rolls back to the confirmed factor', async () => {
  let fail = false;
  const bridge = (...args) => Promise.resolve(fail
    ? { ok: false, factor: 1.1 }
    : { ok: true, factor: args.length ? args[0] : 1.1 });
  const controller = createDesktopZoomController({ bridge, ...fakeTimers() });
  await controller.load();
  fail = true;
  controller.handleWheel(notch(true));
  assert.equal(controller.getSnapshot().factor, 1.25);
  await flush();
  assert.equal(controller.getSnapshot().factor, 1.1);
  assert.equal(controller.getSnapshot().visible, false);
});

assert.equal(DESKTOP_ZOOM_HIDE_DELAY_MS, 1500);
