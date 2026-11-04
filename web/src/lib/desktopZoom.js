// 桌面版 Ctrl+滚轮页面缩放。
//
// 浏览器自带的缩放在桌面壳里是关着的(WebView2 IsZoomControlEnabled=FALSE),
// browserDefaults 在 window 冒泡阶段拦下没被内部预览消费的 Ctrl+滚轮,交给这里:
// 按固定档位换算下一个比例,经 aceDesktop_zoomFactor 让 native 设置 WebView 的
// ZoomFactor(native 进程内记住、退出时写回 state.json),同时弹出比例浮层
// (DesktopZoomIndicator)。没有桥接(浏览器直连 / 不支持的平台)时只拦截不缩放,
// 与改动前一致。

export const DESKTOP_ZOOM_LEVELS = Object.freeze([
  0.5, 0.67, 0.75, 0.8, 0.9, 1, 1.1, 1.25, 1.5, 1.75, 2, 2.5, 3,
]);
export const DESKTOP_ZOOM_HIDE_DELAY_MS = 1500;

const EPSILON = 0.001;
// 鼠标滚轮每格 wheelDelta 是 ±120(与页面缩放无关);触控板 / 高精度滚轮给的是
// 小步像素增量,累积到一格的量再换一档,否则轻轻一推就连跳好几档。
const WHEEL_NOTCH = 120;
const PIXEL_STEP_DELTA = 50;
const GESTURE_IDLE_MS = 300;

const sameFactor = (a, b) => Math.abs(a - b) < EPSILON;

export function isDefaultDesktopZoom(factor) {
  return sameFactor(Number(factor), 1);
}

export function desktopZoomPercent(factor) {
  return `${Math.round(Number(factor) * 100)}%`;
}

// 从当前比例往 direction(+1 放大 / -1 缩小)走一档;不在档位上的值走到相邻档位,到头不动。
export function stepDesktopZoom(current, direction) {
  const value = Number.isFinite(Number(current)) ? Number(current) : 1;
  if (direction > 0) {
    return DESKTOP_ZOOM_LEVELS.find((level) => level > value + EPSILON)
      ?? DESKTOP_ZOOM_LEVELS[DESKTOP_ZOOM_LEVELS.length - 1];
  }
  if (direction < 0) {
    return [...DESKTOP_ZOOM_LEVELS].reverse().find((level) => level < value - EPSILON)
      ?? DESKTOP_ZOOM_LEVELS[0];
  }
  return value;
}

// 一次滚轮事件 → 是否换档。返回 { accumulator, direction },direction 为 0 表示还不够一档。
export function nextWheelZoom(accumulator, event, now) {
  const deltaY = Number(event?.deltaY) || 0;
  if (!deltaY) return { accumulator, direction: 0 };
  const direction = deltaY < 0 ? 1 : -1;
  const notch = Math.abs(Number(event?.wheelDeltaY) || 0);
  if (event?.deltaMode === 1 || event?.deltaMode === 2 || (notch >= WHEEL_NOTCH && notch % WHEEL_NOTCH === 0)) {
    return { accumulator: null, direction };
  }
  const continuing = accumulator
    && now - accumulator.at <= GESTURE_IDLE_MS
    && Math.sign(accumulator.delta) === Math.sign(deltaY);
  const delta = (continuing ? accumulator.delta : 0) + deltaY;
  if (Math.abs(delta) < PIXEL_STEP_DELTA) return { accumulator: { delta, at: now }, direction: 0 };
  return { accumulator: null, direction };
}

export function createDesktopZoomController({
  bridge = null,
  now = () => Date.now(),
  setTimer = (fn, ms) => setTimeout(fn, ms),
  clearTimer = (id) => clearTimeout(id),
  hideDelayMs = DESKTOP_ZOOM_HIDE_DELAY_MS,
} = {}) {
  let snapshot = Object.freeze({ factor: 1, visible: false, available: typeof bridge === 'function' });
  let confirmed = 1;
  let wheel = null;
  let pinned = false;
  let hideTimer = null;
  let requestSeq = 0;
  const listeners = new Set();

  const publish = (patch) => {
    snapshot = Object.freeze({ ...snapshot, ...patch });
    for (const listener of listeners) listener();
  };
  const cancelHide = () => {
    if (hideTimer != null) clearTimer(hideTimer);
    hideTimer = null;
  };
  const scheduleHide = () => {
    cancelHide();
    if (pinned) return;
    hideTimer = setTimer(() => {
      hideTimer = null;
      publish({ visible: false });
    }, hideDelayMs);
  };
  const show = () => {
    if (!snapshot.visible) publish({ visible: true });
    scheduleHide();
  };
  const disable = () => {
    cancelHide();
    publish({ factor: confirmed, visible: false, available: false });
  };
  const callBridge = (...args) => {
    try {
      return Promise.resolve(bridge(...args));
    } catch (error) {
      return Promise.reject(error);
    }
  };

  // 先更新浮层再等 native 回报,快速滚动时每一格都立即可见;回报乱序时只认最后一次请求。
  const apply = (factor) => {
    const id = ++requestSeq;
    publish({ factor, visible: true });
    scheduleHide();
    callBridge(factor).then((result) => {
      if (id !== requestSeq) return;
      const applied = Number(result?.factor);
      if (!result?.ok || !Number.isFinite(applied)) {
        disable();
        return;
      }
      confirmed = applied;
      if (!sameFactor(applied, snapshot.factor)) publish({ factor: applied });
    }, () => {
      if (id === requestSeq) disable();
    });
  };

  return {
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    getSnapshot: () => snapshot,

    // 启动时读一次 native 当前比例(上次退出时保存的值已在页面加载前套用)。
    load() {
      if (!snapshot.available) return Promise.resolve(snapshot);
      const id = requestSeq;
      return callBridge().then((result) => {
        const factor = Number(result?.factor);
        if (!result?.ok || !Number.isFinite(factor)) {
          disable();
        } else if (id === requestSeq) {
          confirmed = factor;
          publish({ factor });
        }
        return snapshot;
      }, () => {
        disable();
        return snapshot;
      });
    },

    handleWheel(event) {
      if (!snapshot.available) return;
      const result = nextWheelZoom(wheel, event, now());
      wheel = result.accumulator;
      if (!result.direction) return;
      const next = stepDesktopZoom(snapshot.factor, result.direction);
      if (sameFactor(next, snapshot.factor)) {
        show();
        return;
      }
      apply(next);
    },

    reset() {
      if (!snapshot.available) return;
      if (isDefaultDesktopZoom(snapshot.factor)) {
        show();
        return;
      }
      apply(1);
    },

    // 指针停在浮层上或焦点在重置按钮上时不自动消失。
    setPinned(value) {
      pinned = !!value;
      if (pinned) cancelHide();
      else if (snapshot.visible) scheduleHide();
    },
  };
}

let singleton = null;

export function desktopZoomController(win = globalThis.window) {
  if (!singleton) {
    const bridge = typeof win?.aceDesktop_zoomFactor === 'function'
      ? (...args) => win.aceDesktop_zoomFactor(...args)
      : null;
    singleton = createDesktopZoomController({ bridge });
  }
  return singleton;
}

// main.jsx 在挂载前调用:建好全局控制器并读当前比例,返回给 browserDefaults 的滚轮处理。
export function installDesktopZoom(win = globalThis.window) {
  const controller = desktopZoomController(win);
  controller.load();
  return (event) => controller.handleWheel(event);
}
