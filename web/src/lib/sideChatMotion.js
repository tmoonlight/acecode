// 侧边聊天窗口的打开 / 最小化动画:打开时从顶栏按钮放大出来,最小化时缩回按钮。
// 变换一律以窗口左上角为原点(transform-origin: 0 0),把窗口当前布局矩形映射到目标矩形。

export const SIDE_CHAT_ENTER_MS = 300;
export const SIDE_CHAT_EXIT_MS = 240;
export const SIDE_CHAT_FADE_MS = 150;
export const SIDE_CHAT_ENTER_EASING = 'cubic-bezier(0.22, 1, 0.36, 1)';
export const SIDE_CHAT_EXIT_EASING = 'cubic-bezier(0.4, 0, 0.8, 0.4)';

// 没有可用按钮时(按钮未渲染 / 被收起 / 滚出视口)退回到原地轻微缩放 + 淡入淡出。
const FALLBACK_SCALE = 0.94;

const finite = (value) => typeof value === 'number' && Number.isFinite(value);
const round = (value, digits) => {
  const factor = 10 ** digits;
  const rounded = Math.round(value * factor) / factor;
  return Object.is(rounded, -0) ? 0 : rounded;
};

function normalizedRect(rect) {
  if (!rect || !finite(rect.left) || !finite(rect.top) || !finite(rect.width) || !finite(rect.height)) return null;
  if (rect.width <= 0 || rect.height <= 0) return null;
  return { left: rect.left, top: rect.top, width: rect.width, height: rect.height };
}

// 按钮必须真实可见且至少一部分落在视口内,否则动画会飞向看不见的位置。
export function usableSideChatLauncherRect(rect, viewport) {
  const launcher = normalizedRect(rect);
  const area = normalizedRect(viewport);
  if (!launcher || !area) return null;
  const overlapsX = launcher.left < area.left + area.width && launcher.left + launcher.width > area.left;
  const overlapsY = launcher.top < area.top + area.height && launcher.top + launcher.height > area.top;
  return overlapsX && overlapsY ? launcher : null;
}

// uniform:等比缩放并居中到目标矩形内(从按钮长出来时不变形);否则精确贴合目标矩形
// (打断进行中的动画时接住当前画面)。
export function sideChatRectTransform(box, target, { uniform = false } = {}) {
  const from = normalizedRect(box);
  const to = normalizedRect(target);
  if (!from || !to) return null;
  let scaleX = to.width / from.width;
  let scaleY = to.height / from.height;
  if (uniform) scaleX = scaleY = Math.min(scaleX, scaleY);
  const translateX = to.left + (to.width - from.width * scaleX) / 2 - from.left;
  const translateY = to.top + (to.height - from.height * scaleY) / 2 - from.top;
  return `translate(${round(translateX, 2)}px, ${round(translateY, 2)}px) `
    + `scale(${round(scaleX, 4)}, ${round(scaleY, 4)})`;
}

function fallbackTransform(box) {
  const rect = normalizedRect(box);
  if (!rect) return 'none';
  return sideChatRectTransform(rect, {
    left: rect.left + (rect.width * (1 - FALLBACK_SCALE)) / 2,
    top: rect.top + (rect.height * (1 - FALLBACK_SCALE)) / 2,
    width: rect.width * FALLBACK_SCALE,
    height: rect.height * FALLBACK_SCALE,
  }) || 'none';
}

// 动画的「另一端」:优先接住被打断动画的当前画面,其次是按钮,最后原地缩放。
function edgeTransform(box, { launcherRect, currentRect }) {
  if (currentRect) {
    const current = sideChatRectTransform(box, currentRect);
    if (current) return current;
  }
  if (launcherRect) {
    const launcher = sideChatRectTransform(box, launcherRect, { uniform: true });
    if (launcher) return launcher;
  }
  return fallbackTransform(box);
}

/**
 * @param {'enter'|'exit'} kind
 * @param {{
 *   box: {left:number,top:number,width:number,height:number},
 *   launcherRect?: object|null,
 *   currentRect?: object|null,
 *   currentOpacity?: number|null,
 *   reducedMotion?: boolean,
 * }} options
 * @returns {{ keyframes: Keyframe[], duration: number, easing: string, usesLauncher: boolean }}
 *   usesLauncher:动画的一端是顶栏按钮(最小化结束后按钮要弹一下)。
 */
export function sideChatMotionPlan(kind, {
  box,
  launcherRect = null,
  currentRect = null,
  currentOpacity = null,
  reducedMotion = false,
} = {}) {
  const entering = kind === 'enter';
  const startOpacity = finite(currentOpacity) ? Math.max(0, Math.min(1, currentOpacity)) : (entering ? 0 : 1);
  if (reducedMotion) {
    return {
      keyframes: [{ opacity: startOpacity }, { opacity: entering ? 1 : 0 }],
      duration: SIDE_CHAT_FADE_MS,
      easing: 'linear',
      usesLauncher: false,
    };
  }
  const usesLauncher = !currentRect && Boolean(normalizedRect(launcherRect));
  if (entering) {
    const from = edgeTransform(box, { launcherRect, currentRect });
    return {
      keyframes: [
        { transformOrigin: '0 0', transform: from, opacity: startOpacity, offset: 0 },
        { transformOrigin: '0 0', opacity: 1, offset: 0.35 },
        { transformOrigin: '0 0', transform: 'none', opacity: 1, offset: 1 },
      ],
      duration: SIDE_CHAT_ENTER_MS,
      easing: SIDE_CHAT_ENTER_EASING,
      usesLauncher,
    };
  }
  const from = currentRect ? (sideChatRectTransform(box, currentRect) || 'none') : 'none';
  const to = edgeTransform(box, { launcherRect });
  return {
    keyframes: [
      { transformOrigin: '0 0', transform: from, opacity: startOpacity, offset: 0 },
      { transformOrigin: '0 0', opacity: startOpacity, offset: 0.55 },
      { transformOrigin: '0 0', transform: to, opacity: 0, offset: 1 },
    ],
    duration: SIDE_CHAT_EXIT_MS,
    easing: SIDE_CHAT_EXIT_EASING,
    usesLauncher: Boolean(normalizedRect(launcherRect)),
  };
}

// 窗口缩进按钮后,按钮轻轻弹一下,表示窗口「收」到了这里。
export const SIDE_CHAT_LAUNCHER_PULSE = {
  keyframes: [
    { transform: 'scale(1)' },
    { transform: 'scale(1.18)', offset: 0.45 },
    { transform: 'scale(1)' },
  ],
  duration: 260,
  easing: 'cubic-bezier(0.33, 1, 0.68, 1)',
};
