// InputBar 上方的"排队卡片栈"。
// 排队消息(QUEUED / SENDING / FAILED)从聊天 transcript 中独立出来,
// 作为 InputBar 紧邻上方的垂直卡片列表呈现:每张卡片自带删除按钮,
// FAILED 状态额外露出"重试"按钮;COMPLETED / CANCELLED 完全不显示。
// 设计上参考 Codex 的待发送草稿堆,与已发送 user 气泡视觉明显区分。
//
// 设计约束(取自 design.md):
//  - 空 items 时返回 null,不渲染任何容器
//  - max-height: 30vh + overflow,避免吃掉聊天可见区
//  - 卡片整体不变色 hover,按钮自身才有 hover
//  - SENDING 短暂窗口卡片仍渲染但 opacity-60,接力到 transcript 由 WS 帧驱动

import { clsx } from '../lib/format.js';
import { buildQueueCardItem } from '../lib/queueCardItem.js';
import { VsIcon } from './Icon.jsx';

function QueueCard({ card, onCancel, onRetry, onGuide, guideDisabled }) {
  const { queuedId, content, statusKind, statusLabel, dimmed, showRetry, canGuide } = card;
  return (
    <div
      role="listitem"
      data-queue-card-state={statusKind}
      className={clsx(
        'ace-queue-card relative flex items-center gap-2 pl-4 pr-2 py-2 text-[13px]',
        dimmed && 'ace-queue-card-dimmed',
      )}
    >
      <span
        aria-hidden="true"
        className={clsx(
          'ace-queue-card-indicator',
          statusKind === 'failed' ? 'is-failed' : 'is-queued',
        )}
      />
      <span
        className="ace-queue-card-content flex-1 min-w-0 truncate"
        title={content}
      >
        {content}
      </span>
      <span
        className={clsx(
          'ace-queue-card-status shrink-0 text-[11px]',
          statusKind === 'failed' && 'is-failed',
        )}
        title={statusKind === 'failed' ? statusLabel : undefined}
      >
        {statusLabel}
      </span>
      {showRetry && (
        <button
          type="button"
          aria-label="重试发送"
          onClick={() => onRetry?.(queuedId)}
          className="ace-queue-card-action shrink-0 px-1.5 h-6 rounded text-[11px]"
        >
          重试
        </button>
      )}
      {canGuide && (
        <button
          type="button"
          aria-label="将排队消息插入当前回合"
          onClick={() => onGuide?.(queuedId)}
          disabled={guideDisabled}
          className="ace-queue-card-guide shrink-0 h-6 px-2 rounded-full flex items-center gap-1 text-[11px] disabled:opacity-50 disabled:cursor-not-allowed"
          title="在当前回合的下一次模型调用前加入这条消息"
        >
          <span>插话</span>
          <VsIcon name="glyphUp" size={10} />
        </button>
      )}
      <button
        type="button"
        aria-label="取消排队"
        onClick={() => onCancel?.(queuedId)}
        className="ace-queue-card-close shrink-0 w-6 h-6 rounded flex items-center justify-center"
        title="取消"
      >
        <VsIcon name="close" size={12} />
      </button>
    </div>
  );
}

export function QueueCardList({ items, onCancel, onRetry, onGuide, guideDisabled = false }) {
  const list = Array.isArray(items) ? items : [];
  if (list.length === 0) return null;
  const cards = list.map(buildQueueCardItem).filter((c) => c.queuedId);
  if (cards.length === 0) return null;
  return (
    <div
      role="list"
      aria-label="排队中的待发送消息"
      className="ace-queue-card-strip flex flex-col gap-1.5 px-2.5 pt-2 max-h-[30vh] overflow-y-auto"
    >
      {cards.map((card) => (
        <QueueCard
          key={card.queuedId}
          card={card}
          onCancel={onCancel}
          onRetry={onRetry}
          onGuide={onGuide}
          guideDisabled={guideDisabled}
        />
      ))}
    </div>
  );
}

export default QueueCardList;
