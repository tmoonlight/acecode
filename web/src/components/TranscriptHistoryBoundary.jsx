export function TranscriptHistoryBoundary({ boundaryRef, phase, hiddenCount, hasMore, onEarlier, onAll }) {
  if (!hiddenCount && !hasMore && phase === 'idle') return null;
  const loading = phase === 'loading';
  const failed = phase === 'error' || phase === 'reset';
  return (
    <div
      ref={boundaryRef}
      data-transcript-history-boundary="true"
      aria-busy={loading}
      className="flex min-h-10 flex-wrap items-center justify-center gap-x-3 gap-y-1 py-1.5 text-[12px] text-fg-mute"
    >
      <span role="status" aria-live="polite" className="contents">
        {loading && (
          <span className="inline-flex items-center gap-2 py-1">
            <span className="ace-spinner motion-reduce:animate-none" aria-hidden="true" />
            正在加载更早的消息…
          </span>
        )}
        {failed && (
          <span>{phase === 'reset' ? '会话历史已更新，请重试' : '更早的消息加载失败'}</span>
        )}
      </span>
      {!loading && (
        <button
          type="button"
          onClick={onEarlier}
          className="px-3 py-1 rounded-full border border-border bg-surface hover:bg-surface-hi hover:text-fg transition focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
        >
          {failed ? '重试' : hiddenCount > 0 ? `显示更早的 ${hiddenCount} 条消息` : '显示更早的消息'}
        </button>
      )}
      <button
        type="button"
        onClick={onAll}
        disabled={loading}
        className="hover:text-fg transition underline-offset-2 hover:underline disabled:opacity-50 disabled:cursor-default focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-accent/25"
      >
        显示全部
      </button>
    </div>
  );
}
