// 回复中按 Enter 插进当前回合、模型还没读到的消息(插话)。画在对话末尾:淡化的用户气泡,
// 下面是「立即发送」(打断当前回合、马上发出)和撤回。模型读到后正式消息出现在对话里,
// 这里的气泡随之消失;回合结束仍没读到的转回输入框上方的排队卡片。
import { VsIcon } from './Icon.jsx';

export function SteeredMessages({ items = [], onSendNow, onWithdraw }) {
  return items.map((item) => {
    const queuedId = item.queued?.id || item.id;
    const sendingNow = item.queued?.steerMode === 'now';
    return (
      <div
        key={queuedId}
        className="ace-chat-row flex flex-col"
        data-chat-row="true"
        data-chat-kind="steered"
        data-steered-message={queuedId}
      >
        <div className="self-end min-w-0 max-w-[70%] flex flex-col items-end gap-0.5">
          <div className="ace-user-message-bubble ace-steered-message-bubble opacity-75 min-w-0 max-w-full px-3.5 py-2 rounded-[14px] rounded-br-[4px] bg-accent-bg border border-dashed border-accent-soft text-fg text-[13px] leading-[1.5] whitespace-pre-wrap break-words line-clamp-6">
            {item.content}
          </div>
          <div className="min-h-6 flex items-center justify-end gap-1 mr-1">
            <button
              type="button"
              disabled={sendingNow}
              onClick={() => onSendNow?.(queuedId)}
              title="打断当前回合，立刻发送"
              className="h-6 px-2 rounded-md text-[12px] text-accent transition hover:bg-surface-hi disabled:text-fg-mute disabled:cursor-wait disabled:hover:bg-transparent"
              data-steered-send-now="true"
            >
              立即发送
            </button>
            <button
              type="button"
              disabled={sendingNow}
              onClick={() => onWithdraw?.(queuedId)}
              title="撤回"
              aria-label="撤回"
              className="w-6 h-6 rounded-md flex items-center justify-center text-fg-mute transition hover:bg-surface-hi hover:text-fg disabled:opacity-40"
              data-steered-withdraw="true"
            >
              <VsIcon name="close" size={13} />
            </button>
          </div>
        </div>
      </div>
    );
  });
}
