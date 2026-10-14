import { useEffect, useId, useState } from 'react';
import { usePreference } from '../lib/usePreference.js';
import {
  DEFAULT_SIDE_CHAT_RETENTION_MINUTES,
  isSideChatRetentionMinutes,
  SIDE_CHAT_RETENTION_KEY,
} from '../lib/sideChatRetention.js';

export function SideChatRetentionSettings() {
  const inputId = useId();
  const [minutes, setMinutes] = usePreference(
    SIDE_CHAT_RETENTION_KEY, DEFAULT_SIDE_CHAT_RETENTION_MINUTES, isSideChatRetentionMinutes,
  );
  const [input, setInput] = useState(() => String(minutes));
  useEffect(() => setInput(String(minutes)), [minutes]);

  return (
    <div className="flex flex-wrap items-center justify-between gap-3 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
      <div className="flex-1 min-w-0">
        <label htmlFor={inputId} className="text-[13px] font-normal text-fg">侧边聊天存续时间</label>
        <p id={`${inputId}-description`} className="text-[11px] text-fg-mute mt-0.5">
          从最后一次侧聊结束开始计时，到期后切回该会话时清空；正在查看时不会自动消失。
        </p>
      </div>
      <div className="flex items-center gap-2 shrink-0">
        <input
          id={inputId}
          type="number"
          min="1"
          step="1"
          value={input}
          aria-describedby={`${inputId}-description`}
          onChange={(event) => {
            const next = event.target.value;
            setInput(next);
            if (isSideChatRetentionMinutes(Number(next))) setMinutes(Number(next));
          }}
          onBlur={() => setInput(String(minutes))}
          onKeyDown={(event) => { if (event.key === 'Enter') event.currentTarget.blur(); }}
          className="w-20 h-7 px-2 text-[12px] text-center rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition"
        />
        <span className="text-[12px] text-fg-mute">分钟</span>
      </div>
    </div>
  );
}
