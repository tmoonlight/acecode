import { clsx } from '../lib/format.js';
import { normalizeBusySendMode } from '../lib/inputBarState.js';
import { Toggle } from './Modal.jsx';

// 设置 > 常规 > 输入框。两项都存在 daemon 的 web_ui 里(follow_up_prompts / busy_send_mode),
// 由 App 经外观偏好控制器保存:桌面版每次启动端口都变,浏览器本地存储留不住。
const BUSY_SEND_MODE_OPTIONS = [
  { key: 'queue', label: '排队' },
  { key: 'steer', label: '插话' },
];

export function ComposerSettings({
  followUpPrompts = true,
  onFollowUpPromptsChange = () => {},
  busySendMode = 'steer',
  onBusySendModeChange = () => {},
}) {
  const mode = normalizeBusySendMode(busySendMode);
  return (
    <>
      <div className="text-[14px] font-semibold mb-1">输入框</div>
      <div className="flex items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0">
          <div className="text-[13px] font-normal text-fg">显示预测消息</div>
        </div>
        <Toggle on={followUpPrompts} onChange={onFollowUpPromptsChange} ariaLabel="显示预测消息" />
      </div>
      <div className="flex flex-wrap items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div className="min-w-0">
          <div id="settings-busy-send-mode-label" className="text-[13px] font-normal text-fg">后续消息处理方式</div>
          <div className="text-[11px] text-fg-mute mt-0.5">回复中按 Enter 生效，Ctrl+Enter 执行另一种</div>
        </div>
        <div
          role="group"
          aria-labelledby="settings-busy-send-mode-label"
          className="grid grid-cols-2 gap-1 p-0.5 rounded-lg border border-border bg-surface-alt shrink-0"
        >
          {BUSY_SEND_MODE_OPTIONS.map((opt) => {
            const active = mode === opt.key;
            return (
              <button
                key={opt.key}
                type="button"
                aria-pressed={active}
                onClick={() => { if (!active) onBusySendModeChange(opt.key); }}
                className={clsx(
                  'h-7 min-w-14 px-3 rounded-md text-[12px] font-normal transition',
                  active ? 'bg-accent text-white shadow-sm' : 'text-fg-2 hover:bg-surface-hi hover:text-fg',
                )}
              >
                {opt.label}
              </button>
            );
          })}
        </div>
      </div>
    </>
  );
}
