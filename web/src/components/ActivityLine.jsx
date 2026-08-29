import { clsx } from '../lib/format.js';

/**
 * 对话流中唯一的被动活动首行。
 *
 * loading、运行/完成工具、工具汇总和处理摘要都复用这一个固定尺寸外壳；
 * 调用方只替换图标、文案和尾部元数据，避免状态切换时换壳或改变首行高度。
 */
export function ActivityLine({
  label = '',
  detail = '',
  icon = null,
  running = false,
  spinnerStatic = false,
  trailing = null,
  expandable = false,
  expanded = false,
  onToggle,
  title,
  ariaLabel,
  live = false,
  preserveLabel = false,
  className = '',
}) {
  const interactive = expandable && typeof onToggle === 'function';
  const activityLabel = label || '正在处理';
  const liveCopyClassName = live ? 'ace-activity-line-live-copy' : '';
  const handleKeyDown = (event) => {
    if (!interactive || (event.key !== 'Enter' && event.key !== ' ')) return;
    event.preventDefault();
    onToggle(event);
  };
  const content = (
    <>
      <span
        className="flex h-4 w-4 shrink-0 items-center justify-center"
        aria-hidden="true"
      >
        {running ? (
          <span className={clsx('ace-spinner h-3 w-3', spinnerStatic && 'ace-spinner-static')} />
        ) : icon}
      </span>
      <span className={clsx(
        'whitespace-nowrap font-medium text-fg-mute group-hover/activity:text-fg',
        preserveLabel ? 'shrink-0' : 'min-w-0 max-w-[62%] truncate',
        liveCopyClassName,
      )}>
        {activityLabel}
        {live && (
          <span className="ace-activity-line-shimmer-sweep" aria-hidden="true">
            <span className="ace-activity-line-shimmer-highlight">{activityLabel}</span>
          </span>
        )}
      </span>
      {detail && (
        <span className="min-w-0 truncate text-fg-mute" title={String(detail)}>
          · {detail}
        </span>
      )}
      {expandable && (
        <span
          className="ace-activity-line-chevron flex h-4 w-4 shrink-0 items-center justify-center"
          aria-hidden="true"
        >
          <svg
            xmlns="http://www.w3.org/2000/svg"
            width="16"
            height="16"
            viewBox="0 0 16 16"
            fill="none"
            className="block shrink-0"
            style={{ transform: `rotate(${expanded ? 0 : -90}deg)` }}
            aria-hidden="true"
          >
            <path
              d="M4 6L8 10L12 6"
              stroke="currentColor"
              strokeWidth="1.2"
              strokeLinecap="round"
              strokeLinejoin="round"
            />
          </svg>
        </span>
      )}
      <span className="min-w-0 flex-1" aria-hidden="true" />
      {trailing && (
        <span className="flex shrink-0 items-center gap-1.5 text-fg-mute">
          {trailing}
        </span>
      )}
    </>
  );
  const sharedClassName = clsx(
    'group/activity flex h-7 w-full min-w-0 max-w-[88%] items-center gap-1.5 overflow-hidden whitespace-nowrap px-0 py-0 text-left text-fg-mute',
    interactive && 'cursor-pointer rounded-sm outline-none transition-colors focus-visible:ring-1 focus-visible:ring-accent/60',
  );

  return (
    <div
      className={clsx('ace-activity-line ace-tool-call-text my-0.5 min-w-0', className)}
      data-unified-activity-line="true"
      data-activity-live={live ? 'true' : 'false'}
    >
      <div
        className={sharedClassName}
        role={interactive ? 'button' : (live ? 'status' : undefined)}
        tabIndex={interactive ? 0 : undefined}
        onClick={interactive ? onToggle : undefined}
        onKeyDown={interactive ? handleKeyDown : undefined}
        aria-expanded={interactive ? expanded : undefined}
        aria-live={live ? 'polite' : undefined}
        aria-atomic={live ? 'true' : undefined}
        aria-label={interactive ? (ariaLabel || (expanded ? '收起详情' : '展开详情')) : undefined}
        title={title || (interactive ? (expanded ? '收起详情' : '展开详情') : undefined)}
        data-activity-expandable={expandable ? 'true' : 'false'}
        data-activity-title-anchor="true"
      >
        {content}
      </div>
    </div>
  );
}
