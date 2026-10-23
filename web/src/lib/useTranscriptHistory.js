import { useLayoutEffect, useMemo, useRef, useState } from 'react';
import { revealEarlierAnchorKey } from './transcriptWindow.js';
import { createTranscriptHistoryController } from './transcriptHistoryController.js';

const afterPaint = () => new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));

export function useTranscriptHistory(options) {
  const latest = useRef(options);
  latest.current = options;
  const boundaryRef = useRef(null);
  const [status, setStatus] = useState(null);
  const controller = useMemo(() => createTranscriptHistoryController({
    getViewport: () => latest.current.scrollRef.current,
    getBoundary: () => boundaryRef.current,
    getSnapshot: () => ({
      ...latest.current.getState(),
      hiddenCount: latest.current.hiddenCount,
    }),
    onPhase: (phase) => setStatus({ controller, phase }),
    onReview: () => latest.current.onReview(),
    afterPaint,
    loadPage: () => options.loadEarlier(null, { detailed: true, silent: true }),
    // 用户主动展开的窗口带 pinned:停在底部跟随时的自动封顶会跳过它,
    // 直到用户回到底部或发出新消息(ChatView 解除)。
    revealLocal: () => {
      const current = latest.current;
      current.setWindow((previous) => previous.sid === options.sid && previous.anchorKey
        ? { sid: options.sid, anchorKey: revealEarlierAnchorKey(current.items, previous.anchorKey), pinned: true }
        : previous);
    },
    showAll: () => latest.current.setWindow((previous) => previous.sid === options.sid
      ? { sid: options.sid, anchorKey: null, pinned: true } : previous),
  }), [options.sid, options.loadEarlier]);
  useLayoutEffect(() => {
    controller.activate();
    return () => controller.dispose();
  }, [controller]);
  useLayoutEffect(() => { controller.preserveAnchor(); }, [controller, options.items, options.anchorKey, status]);
  return {
    controller,
    boundaryRef,
    phase: status?.controller === controller ? status.phase : 'idle',
  };
}
