import { useEffect, useRef, useState } from 'react';
import {
  createSlowPendingGate,
  sessionStartMaskApplies,
  sessionStartMaskKind,
} from './sessionStartMask.js';

// ChatView 用它登记「正在新建会话 / 正在建 worktree」。返回的对象身份稳定:
//   begin(marker)            开始等待,返回 token(可自带 token)
//   attachSession(token, id) 会话建好,记下它的 id
//   end(token)               等待结束;token 不是当前这次的就忽略
//   isBlocking()             当前界面是否正被这次启动占着(发送 / 停止入口用它早退,
//                            遮罩出现之前同样成立)
//   kind                     '' | 'session' | 'worktree',非空时画遮罩
export function useSessionStartMask({ sessionId = '', auxiliaryPage = false } = {}) {
  const [state, setState] = useState({ marker: null, visible: false });
  const pendingRef = useRef(null);
  // 收尾停留期间 marker 已清空,遮罩仍要知道自己属于哪次启动。
  const lastRef = useRef(null);
  // 界面已经显示过新会话的那次启动(token)。此后回到首页不再算这次启动的界面。
  const arrivedRef = useRef(null);
  const viewRef = useRef({ sessionId, auxiliaryPage });
  viewRef.current = { sessionId, auxiliaryPage };

  const apiRef = useRef(null);
  if (!apiRef.current) {
    const gate = createSlowPendingGate({
      onChange: (visible) => setState((prev) => ({ ...prev, visible })),
    });
    const commit = (marker) => {
      pendingRef.current = marker;
      if (marker) lastRef.current = marker;
      gate.update(!!marker);
      setState((prev) => ({ ...prev, marker }));
    };
    apiRef.current = {
      gate,
      begin({ token = {}, home = false, worktree = false, sessionId: startSessionId = '' } = {}) {
        commit({ token, home: !!home, worktree: !!worktree, sessionId: String(startSessionId || '') });
        return token;
      },
      attachSession(token, id) {
        const current = pendingRef.current;
        if (!current || current.token !== token) return;
        commit({ ...current, sessionId: String(id || '') });
      },
      end(token) {
        if (!pendingRef.current || pendingRef.current.token !== token) return;
        commit(null);
      },
      isBlocking() {
        const marker = pendingRef.current;
        return sessionStartMaskApplies(marker, {
          ...viewRef.current,
          arrived: !!marker && arrivedRef.current === marker.token,
        });
      },
    };
  }
  useEffect(() => () => apiRef.current.gate.dispose(), []);

  const shown = state.marker || lastRef.current;
  if (shown?.sessionId && sessionId === shown.sessionId) arrivedRef.current = shown.token;
  const applies = state.visible && sessionStartMaskApplies(shown, {
    sessionId,
    auxiliaryPage,
    arrived: !!shown && arrivedRef.current === shown.token,
  });
  const { begin, attachSession, end, isBlocking } = apiRef.current;
  return { begin, attachSession, end, isBlocking, kind: applies ? sessionStartMaskKind(shown) : '' };
}
