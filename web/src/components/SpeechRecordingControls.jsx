import { useEffect, useRef, useState } from 'react';
import { VsIcon } from './Icon.jsx';

export function SpeechRecordingControls({ speech, className = '' }) {
  const wave = useRef(null);
  const [count, setCount] = useState(48);
  useEffect(() => {
    const measure = () => setCount(Math.max(8, Math.min(180, Math.floor((wave.current?.clientWidth || 288) / 6))));
    measure();
    if (typeof ResizeObserver !== 'function') return undefined;
    const observer = new ResizeObserver(measure);
    observer.observe(wave.current);
    return () => observer.disconnect();
  }, []);
  const levels = [...Array(count).fill(0), ...speech.levels].slice(-count);
  const recording = speech.phase === 'recording';
  const button = 'w-7 h-7 rounded-full shrink-0 flex items-center justify-center text-fg hover:bg-surface-hi focus-visible:outline-2 focus-visible:outline-accent disabled:opacity-40 disabled:cursor-default';
  return <div data-speech-recording={speech.phase} className={'flex items-center gap-2 min-w-0 h-full ' + className}>
    <button type="button" className={button} aria-label="取消听写" title="取消听写" onClick={speech.cancel}>
      <VsIcon name="close" size={16} />
    </button>
    <div ref={wave} data-speech-wave className="flex-1 min-w-0 h-6 flex items-center justify-between gap-px overflow-hidden"
      role="status" aria-label={recording ? '正在录音' : speech.phase === 'requesting' ? '正在请求麦克风权限' : '正在识别语音'}>
      {recording ? levels.map((value, index) => <span key={index} aria-hidden="true"
        className="block shrink-0 rounded-full bg-fg-mute"
        style={{ width: 3, height: 3 + value * 17, opacity: 0.35 + value * 0.65 }} />)
        : <span className="text-[12px] text-fg-mute">{speech.phase === 'requesting' ? '等待麦克风权限…' : '正在识别…'}</span>}
    </div>
    <button type="button" className={button} aria-label="停止录音并插入文字" title="停止录音" disabled={!recording} onClick={() => speech.stop(false)}>
      <VsIcon name="stopFilled" size={14} />
    </button>
    <button type="button" className="ace-composer-send w-7 h-7 rounded-full shrink-0 flex items-center justify-center bg-accent text-white disabled:opacity-40 focus-visible:outline-2 focus-visible:outline-accent"
      aria-label="识别并发送" title="识别并发送" disabled={!recording} onClick={() => speech.stop(true)}>
      <VsIcon name="send" size={14} mono={false} className="ace-icon-on-accent" />
    </button>
  </div>;
}
