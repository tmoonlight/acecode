import assert from 'node:assert/strict';
import {
  SIDE_CHAT_ENTER_MS,
  SIDE_CHAT_EXIT_MS,
  SIDE_CHAT_FADE_MS,
  sideChatMotionPlan,
  sideChatRectTransform,
  usableSideChatLauncherRect,
} from './sideChatMotion.js';

const viewport = { left: 0, top: 0, width: 1280, height: 900 };
const windowBox = { left: 668, top: 56, width: 600, height: 620 };
const launcher = { left: 1100, top: 20, width: 28, height: 28 };

// 场景:顶栏按钮可见。期望:原样返回按钮矩形。
assert.deepEqual(usableSideChatLauncherRect(launcher, viewport), launcher);
// 场景:按钮被收起(0 尺寸)、滚出视口或数据不完整。期望:不用它,退回原地缩放,
// 否则窗口会飞向看不见的位置。
assert.equal(usableSideChatLauncherRect({ ...launcher, width: 0 }, viewport), null);
assert.equal(usableSideChatLauncherRect({ ...launcher, left: 1400 }, viewport), null);
assert.equal(usableSideChatLauncherRect({ ...launcher, top: -40 }, viewport), null);
assert.equal(usableSideChatLauncherRect({ left: 1, top: 1, width: 2 }, viewport), null);
assert.equal(usableSideChatLauncherRect(null, viewport), null);

// 场景:窗口从按钮长出来。期望:等比缩放(min(28/600, 28/620) = 0.0452),
// 缩小后的窗口中心落在按钮中心,不被拉扁。
const fromLauncher = sideChatRectTransform(windowBox, launcher, { uniform: true });
assert.equal(fromLauncher, 'translate(432.45px, -36px) scale(0.0452, 0.0452)');
{
  const scale = 28 / 620;
  const centerX = windowBox.left + 432.45 + (windowBox.width * scale) / 2;
  const centerY = windowBox.top - 36 + (windowBox.height * scale) / 2;
  assert.ok(Math.abs(centerX - (launcher.left + launcher.width / 2)) < 0.05);
  assert.ok(Math.abs(centerY - (launcher.top + launcher.height / 2)) < 0.05);
}
// 场景:接住被打断动画的当前画面。期望:精确贴合目标矩形(宽高各自缩放)。
assert.equal(
  sideChatRectTransform(windowBox, { left: 768, top: 106, width: 300, height: 155 }),
  'translate(100px, 50px) scale(0.5, 0.25)',
);
// 场景:同一矩形。期望:恒等变换,不产生 -0。
assert.equal(sideChatRectTransform(windowBox, windowBox), 'translate(0px, 0px) scale(1, 1)');
assert.equal(sideChatRectTransform({ ...windowBox, width: 0 }, launcher), null);

// 场景:点击按钮打开。期望:从按钮的等比缩放位置透明起步,前 35% 淡入,结束于原位不透明;
// 动画以左上角为原点,和变换的计算口径一致。
{
  const plan = sideChatMotionPlan('enter', { box: windowBox, launcherRect: launcher });
  assert.equal(plan.duration, SIDE_CHAT_ENTER_MS);
  assert.equal(plan.usesLauncher, true);
  assert.deepEqual(plan.keyframes, [
    { transformOrigin: '0 0', transform: fromLauncher, opacity: 0, offset: 0 },
    { transformOrigin: '0 0', opacity: 1, offset: 0.35 },
    { transformOrigin: '0 0', transform: 'none', opacity: 1, offset: 1 },
  ]);
}

// 场景:点击右上角最小化。期望:原位起步,过半后才开始变淡,最终缩进按钮并完全透明;
// usesLauncher 让按钮在窗口落进去后弹一下。
{
  const plan = sideChatMotionPlan('exit', { box: windowBox, launcherRect: launcher });
  assert.equal(plan.duration, SIDE_CHAT_EXIT_MS);
  assert.equal(plan.usesLauncher, true);
  assert.deepEqual(plan.keyframes, [
    { transformOrigin: '0 0', transform: 'none', opacity: 1, offset: 0 },
    { transformOrigin: '0 0', opacity: 1, offset: 0.55 },
    { transformOrigin: '0 0', transform: fromLauncher, opacity: 0, offset: 1 },
  ]);
}

// 场景:/btw 打开或按钮不可见。期望:原地从 94% 缩放淡入,不假装从按钮出来,按钮也不弹。
{
  const plan = sideChatMotionPlan('enter', { box: windowBox, launcherRect: null });
  assert.equal(plan.usesLauncher, false);
  assert.equal(plan.keyframes[0].transform, 'translate(18px, 18.6px) scale(0.94, 0.94)');
  const exit = sideChatMotionPlan('exit', { box: windowBox });
  assert.equal(exit.usesLauncher, false);
  assert.equal(exit.keyframes.at(-1).transform, 'translate(18px, 18.6px) scale(0.94, 0.94)');
}

// 场景:最小化动画播到一半又点了打开。期望:从当前画面(缩到一半的矩形、当前透明度)
// 接着放大,而不是跳回按钮重新开始;此时不算「从按钮出来」。
{
  const current = { left: 900, top: 40, width: 150, height: 155 };
  const plan = sideChatMotionPlan('enter', {
    box: windowBox,
    launcherRect: launcher,
    currentRect: current,
    currentOpacity: 0.6,
  });
  assert.equal(plan.usesLauncher, false);
  assert.equal(plan.keyframes[0].transform, sideChatRectTransform(windowBox, current));
  assert.equal(plan.keyframes[0].opacity, 0.6);
}

// 场景:打开动画还没播完就点了最小化。期望:从当前画面缩回按钮,透明度从当前值起算。
{
  const current = { left: 1000, top: 30, width: 300, height: 310 };
  const plan = sideChatMotionPlan('exit', {
    box: windowBox,
    launcherRect: launcher,
    currentRect: current,
    currentOpacity: 0.8,
  });
  assert.equal(plan.keyframes[0].transform, sideChatRectTransform(windowBox, current));
  assert.equal(plan.keyframes[0].opacity, 0.8);
  assert.equal(plan.keyframes[1].opacity, 0.8);
  assert.equal(plan.keyframes.at(-1).transform, fromLauncher);
}

// 场景:系统开了「减少动态效果」。期望:只做短暂淡入淡出,没有位移和缩放,按钮不弹。
{
  const enter = sideChatMotionPlan('enter', { box: windowBox, launcherRect: launcher, reducedMotion: true });
  assert.deepEqual(enter.keyframes, [{ opacity: 0 }, { opacity: 1 }]);
  assert.equal(enter.duration, SIDE_CHAT_FADE_MS);
  assert.equal(enter.usesLauncher, false);
  const exit = sideChatMotionPlan('exit', { box: windowBox, launcherRect: launcher, reducedMotion: true });
  assert.deepEqual(exit.keyframes, [{ opacity: 1 }, { opacity: 0 }]);
}
