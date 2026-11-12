// 子代理头像右上角的状态图标,画法照桌面像素办公室(assets/desktop_pet/agent_office_pet.html
// 的 drawOverlays):运行中是三个点轮流跳,结束是一颗星,被中止是红色感叹号。图标本身
// (miniSprite)和办公室是同一份像素模板;这里只算「这一拍画什么、画在哪」,DOM 在 PixelAvatar.jsx。

export const AVATAR_STATUS_TICK_MS = 280;   // 办公室里三个点换一次的间隔

// 图标画布的大小(像素格,已含 1 格描边)和它相对头像右上角的位置(CSS 像素,负数 = 伸出头像)。
// 三个点最宽,往右多伸一些;都只盖住头像右上角本来透明的那一块。
const MARKS = Object.freeze({
  running: Object.freeze({ width: 12, height: 5, right: -5, top: -3 }),
  done: Object.freeze({ width: 7, height: 7, right: -2, top: -3 }),
  stopped: Object.freeze({ width: 3, height: 8, right: -1, top: -3 }),
});

export function avatarStatusMark(status) {
  return MARKS[status] || null;
}

// 这一拍要画的图标:[{ name, color, x, y }],坐标是图标画布里的像素格。
export function avatarStatusSprites(status, tick = 0) {
  if (status === 'running') {
    const lit = ((Number(tick) || 0) % 3 + 3) % 3;
    return [0, 1, 2].map((index) => ({
      name: 'dot',
      color: index === lit ? '#ffffff' : '#b9c3dc',
      x: index * 4,
      y: index === lit ? 0 : 1,   // 亮的那个点抬高一格
    }));
  }
  if (status === 'done') return [{ name: 'star', color: '#ffe14d', x: 0, y: 0 }];
  if (status === 'stopped') return [{ name: 'ex', color: '#ff5a4f', x: 0, y: 0 }];
  return [];
}

// 运行中的小人每 5 拍(约 1.4 秒)点一下头,持续一拍;seed 把各人错开,不会整列一起点。
const NOD_PERIOD_TICKS = 5;
export function avatarNods(status, tick = 0, seed = 0) {
  if (status !== 'running') return false;
  return ((Number(tick) || 0) + (Number(seed) || 0)) % NOD_PERIOD_TICKS === 0;
}

// 点头往下挪多少(CSS 像素):大约是小人的一格,但取整到设备像素。头像是按最近邻放大的
// 像素画,挪半个设备像素会让每一格的粗细重新分配,脸看起来在抖。
export function avatarNodOffsetPx(size, devicePixelRatio = 1) {
  const dpr = Number(devicePixelRatio) > 0 ? Number(devicePixelRatio) : 1;
  return Math.max(1, Math.round((Number(size) || 0) * dpr / 14)) / dpr;
}

// 所有运行中的头像共用一只钟:同一拍里三个点亮的是同一个(办公室也是全场同步)。
// 有人订阅才开定时器,最后一个退订就停,没有运行中的子代理时页面上没有这只定时器。
const listeners = new Set();
let timer = null;
let currentTick = 0;

export function avatarStatusTick() {
  return currentTick;
}

export function subscribeAvatarStatusClock(listener) {
  listeners.add(listener);
  if (timer === null) {
    timer = setInterval(() => {
      currentTick += 1;
      for (const notify of [...listeners]) notify(currentTick);
    }, AVATAR_STATUS_TICK_MS);
  }
  return () => {
    listeners.delete(listener);
    if (listeners.size === 0 && timer !== null) {
      clearInterval(timer);
      timer = null;
    }
  };
}
