// 子代理头像右上角状态图标的单测(画什么、画在哪、什么时候点头、钟什么时候走)。
import assert from 'node:assert/strict';
import { mock, test } from 'node:test';
import {
  AVATAR_STATUS_TICK_MS,
  avatarNodOffsetPx,
  avatarNods,
  avatarStatusMark,
  avatarStatusSprites,
  avatarStatusTick,
  subscribeAvatarStatusClock,
} from './avatarStatus.js';

// 场景:子代理头像右上角按状态画图标,画法照桌面像素办公室。
// 期望:运行中是三个点,每拍亮下一个,亮的那个是白色并抬高一格,另外两个是灰蓝色;
// 已完成是一颗黄色的星,被中止是红色感叹号;没有状态时什么都不画。
test('状态图标:运行中三个点轮流亮,完成是星,中止是红色感叹号', () => {
  const frames = [0, 1, 2, 3].map((tick) => avatarStatusSprites('running', tick));
  for (const [tick, frame] of frames.entries()) {
    assert.equal(frame.length, 3);
    assert.deepEqual(frame.map((dot) => dot.name), ['dot', 'dot', 'dot']);
    assert.deepEqual(frame.map((dot) => dot.x), [0, 4, 8]);
    const lit = tick % 3;
    assert.deepEqual(frame.map((dot) => dot.color),
      [0, 1, 2].map((index) => (index === lit ? '#ffffff' : '#b9c3dc')));
    assert.deepEqual(frame.map((dot) => dot.y), [0, 1, 2].map((index) => (index === lit ? 0 : 1)));
  }
  assert.deepEqual(avatarStatusSprites('done', 7), [{ name: 'star', color: '#ffe14d', x: 0, y: 0 }]);
  assert.deepEqual(avatarStatusSprites('stopped', 7), [{ name: 'ex', color: '#ff5a4f', x: 0, y: 0 }]);
  assert.deepEqual(avatarStatusSprites('', 0), []);
  assert.deepEqual(avatarStatusSprites(undefined), []);
});

// 场景:图标画在一张单独的小画布上,叠在头像右上角。
// 期望:画布正好装下图标(三个点 4 格一个共 12 格宽、再留 1 格给抬高;星 5×5、感叹号 1×6,
// 各加 1 格描边),并且往右、往上伸出头像一点;没有状态就没有画布。
// 阈值:三个点最宽,只伸出 5px。列表里头像和标题之间留了 10px,再伸就压到标题的第一个字。
test('状态图标的画布大小和位置', () => {
  assert.deepEqual(avatarStatusMark('running'), { width: 12, height: 5, right: -5, top: -3 });
  assert.deepEqual(avatarStatusMark('done'), { width: 7, height: 7, right: -2, top: -3 });
  assert.deepEqual(avatarStatusMark('stopped'), { width: 3, height: 8, right: -1, top: -3 });
  assert.equal(avatarStatusMark(''), null);
  assert.equal(avatarStatusMark('unknown'), null);
  for (const status of ['running', 'done', 'stopped']) {
    assert.ok(avatarStatusMark(status).right >= -5, status);
  }
});

// 场景:运行中的小人隔一会儿点一下头(和办公室里读东西时一样),结束后不动。
// 期望:每 5 拍(约 1.4 秒)点一拍;不同的人按各自的 seed 错开;非运行状态永远不点。
test('运行中的头像每 5 拍点一下头,各人错开', () => {
  const nodTicks = (seed) => Array.from({ length: 10 }, (_, tick) => tick)
    .filter((tick) => avatarNods('running', tick, seed));
  assert.deepEqual(nodTicks(0), [0, 5]);
  assert.deepEqual(nodTicks(2), [3, 8]);
  assert.notDeepEqual(nodTicks(0), nodTicks(1));
  for (let tick = 0; tick < 10; tick += 1) {
    assert.equal(avatarNods('done', tick, 0), false);
    assert.equal(avatarNods('stopped', tick, 0), false);
    assert.equal(avatarNods('', tick, 0), false);
  }
});

// 场景:头像是按最近邻放大的像素画(16px 的头像在 150% 缩放的屏幕上是 24 个设备像素)。
// 期望:点头的位移大约是小人的一格,但一定是整数个设备像素。
// 回归(设计时实测):挪半个设备像素会让每一格的粗细重新分配,脸看起来在抖而不是在点头。
test('点头的位移取整到设备像素', () => {
  assert.equal(avatarNodOffsetPx(16, 1), 1);          // 16/14 ≈ 1.14 → 1 个设备像素
  assert.equal(avatarNodOffsetPx(16, 2), 1);          // 2.29 → 2 个设备像素 = 1 CSS 像素
  assert.equal(avatarNodOffsetPx(16, 1.5) * 1.5, 2);  // 1.71 → 2 个设备像素
  assert.equal(avatarNodOffsetPx(36, 1), 3);
  for (const dpr of [1, 1.25, 1.5, 1.75, 2, 3]) {
    const devicePixels = avatarNodOffsetPx(16, dpr) * dpr;
    assert.ok(Math.abs(devicePixels - Math.round(devicePixels)) < 1e-9, `dpr ${dpr}`);
    assert.ok(devicePixels >= 1, `dpr ${dpr}`);
  }
  // 拿不到缩放比时按 1 算,不能算出 0 或 NaN。
  assert.equal(avatarNodOffsetPx(16, 0), 1);
  assert.equal(avatarNodOffsetPx(16, undefined), 1);
});

// 场景:页面上同时有好几个运行中的子代理头像(对话、摘要面板、后台任务页签)。
// 期望:它们共用一只钟,同一拍收到同一个数(三个点同步);有人订阅才有定时器,
// 最后一个退订后定时器停掉,之后不再有回调。
// 回归防护:桌面版常常挂机很久,子代理都结束后页面上不能留着一个空转的定时器。
test('状态钟:共用、同步,没人订阅就停', () => {
  mock.timers.enable({ apis: ['setInterval'] });
  try {
    const first = [];
    const second = [];
    const start = avatarStatusTick();
    const offFirst = subscribeAvatarStatusClock((tick) => first.push(tick));
    mock.timers.tick(AVATAR_STATUS_TICK_MS);
    const offSecond = subscribeAvatarStatusClock((tick) => second.push(tick));
    mock.timers.tick(AVATAR_STATUS_TICK_MS * 2);
    assert.deepEqual(first, [start + 1, start + 2, start + 3]);
    assert.deepEqual(second, [start + 2, start + 3]);
    assert.equal(avatarStatusTick(), start + 3);

    offFirst();
    mock.timers.tick(AVATAR_STATUS_TICK_MS);
    assert.equal(first.length, 3);
    assert.deepEqual(second, [start + 2, start + 3, start + 4]);

    offSecond();
    mock.timers.tick(AVATAR_STATUS_TICK_MS * 5);
    assert.equal(second.length, 3);
    assert.equal(avatarStatusTick(), start + 4);   // 没人订阅时钟不走

    // 再有人订阅时接着走,而不是残留着上一只定时器同时跑两只。
    const again = [];
    const offAgain = subscribeAvatarStatusClock((tick) => again.push(tick));
    mock.timers.tick(AVATAR_STATUS_TICK_MS);
    assert.deepEqual(again, [start + 5]);
    offAgain();
  } finally {
    mock.timers.reset();
  }
});
