// 新建会话(含首条消息里建 worktree)的等待遮罩。
//
// 正常情况下建会话只要几十毫秒,遮罩一闪而过反而碍眼,所以它只在「迟迟没完成」
// 时才出现:等待超过 SESSION_START_MASK_DELAY_MS 才显示,显示后至少停留
// SESSION_START_MASK_MIN_VISIBLE_MS,避免刚出现就消失。
// 遮罩出现之前不拦截输入(一闪而过地吞掉按键比不拦更糟);这段时间只靠发送 / 停止
// 入口的早退保护。

export const SESSION_START_MASK_DELAY_MS = 600;
export const SESSION_START_MASK_MIN_VISIBLE_MS = 400;

// 「等了很久才亮,亮了至少亮一会儿」的开关。update(true/false) 报告是否还在等待,
// onChange(visible) 在可见性真正变化时回调。
export function createSlowPendingGate({
  delayMs = SESSION_START_MASK_DELAY_MS,
  minVisibleMs = SESSION_START_MASK_MIN_VISIBLE_MS,
  onChange = () => {},
  setTimer = (fn, ms) => setTimeout(fn, ms),
  clearTimer = (id) => clearTimeout(id),
  now = () => Date.now(),
} = {}) {
  let pending = false;
  let visible = false;
  let shownAt = 0;
  let timer = null;

  const cancelTimer = () => {
    if (timer === null) return;
    clearTimer(timer);
    timer = null;
  };
  const setVisible = (next) => {
    if (visible === next) return;
    visible = next;
    if (next) shownAt = now();
    onChange(next);
  };

  return {
    isVisible: () => visible,
    update(nextPending) {
      const next = !!nextPending;
      if (next === pending) return;
      pending = next;
      cancelTimer();
      if (pending) {
        // 上一次的收尾停留还没结束又开始等待:保持可见,不重新计时。
        if (visible) return;
        timer = setTimer(() => {
          timer = null;
          setVisible(true);
        }, delayMs);
        return;
      }
      if (!visible) return;
      const remaining = minVisibleMs - (now() - shownAt);
      if (remaining <= 0) {
        setVisible(false);
        return;
      }
      timer = setTimer(() => {
        timer = null;
        setVisible(false);
      }, remaining);
    },
    dispose() {
      cancelTimer();
      pending = false;
      visible = false;
    },
  };
}

// 一次启动的标记:{ token, home, worktree, sessionId }。
//   home      从新建任务首页发起(会话还要先创建)
//   worktree  首条消息带着建 worktree 的意图
//   sessionId 会话建好后填入;在已有的空会话里发起时一开始就有
//
// 遮罩只盖「正在启动的那个会话」所在的界面。等待中用户切到别的会话、定时任务页,
// 或进过新会话后又回到首页开下一个任务,都不该被它挡住。
export function sessionStartMaskApplies(marker, {
  sessionId = '',
  auxiliaryPage = false,
  arrived = false,
} = {}) {
  if (!marker) return false;
  const current = String(sessionId || '');
  if (current) return current === String(marker.sessionId || '');
  return !!marker.home && !auxiliaryPage && !arrived;
}

// 文案:会话建好之前一律是「新建会话」;建好之后还在等的只剩 worktree。
export function sessionStartMaskKind(marker) {
  if (!marker) return '';
  return marker.worktree && marker.sessionId ? 'worktree' : 'session';
}
