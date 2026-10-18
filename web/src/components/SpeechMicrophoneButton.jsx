import { useCallback, useEffect, useRef, useState } from 'react';
import { AnchoredMenu } from './AnchoredMenu.jsx';
import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { listSpeechDevices } from '../lib/speechDevices.js';
import { speechErrorMessage } from '../lib/speechRecognition.js';
import {
  closeDesktopContextMenu,
  CONTEXT_MENU_DELEGATE_EVENT,
  SPEECH_MICROPHONE_CONTEXT_MENU_DELEGATE,
} from '../lib/desktopContextMenu.js';

function MicrophoneMenu({ speech, anchorRef, onClose }) {
  const [devices, setDevices] = useState([]);
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(true);
  const [retry, setRetry] = useState(0);
  useEffect(() => {
    let current;
    const load = (requestPermission) => {
      current?.abort();
      const request = new AbortController(); current = request;
      setLoading(true); setError(null);
      void listSpeechDevices({ signal: request.signal, requestPermission })
        .then((items) => { if (!request.signal.aborted) setDevices(items); })
        .catch((failure) => { if (!request.signal.aborted) setError(failure); })
        .finally(() => { if (!request.signal.aborted) setLoading(false); });
    };
    load(true);
    const changed = () => load(false);
    navigator.mediaDevices?.addEventListener?.('devicechange', changed);
    return () => { current?.abort(); navigator.mediaDevices?.removeEventListener?.('devicechange', changed); };
  }, [retry]);
  return <AnchoredMenu anchorRef={anchorRef} onClose={onClose} width={340} preferredPlacement="above"
    role="menu" aria-label="麦克风配置" className="rounded-lg border border-border bg-surface text-fg p-1.5 ace-shadow">
    <div className="px-2 py-1 text-[12px] text-fg-mute">麦克风</div>
    {loading && <div role="status" className="px-2 py-2 text-[12px] text-fg-mute">加载中</div>}
    {error && <div role="alert" className="px-2 py-2 text-[12px] text-danger">
      {speechErrorMessage(error)} <button type="button" onClick={() => setRetry((value) => value + 1)} className="hover:underline">重试</button>
    </div>}
    {devices.map((device, index) => <button key={device.id} type="button" role="menuitemradio" data-microphone-device={device.id}
      aria-checked={speech.preferences.deviceId === device.id}
      className="w-full flex items-center gap-2 px-2 py-1.5 rounded-md text-left text-[13px] hover:bg-surface-hi focus-visible:outline-2 focus-visible:outline-accent"
      onClick={() => { speech.configureInput({ deviceId: device.id }); onClose(); }}>
      <span className="flex-1 min-w-0 truncate" title={device.label || undefined}>
        {device.label || (device.id === 'default' ? '系统默认' : `麦克风 ${index + 1}`)}
      </span>
      <span className="w-4 shrink-0 text-accent">{speech.preferences.deviceId === device.id && <VsIcon name="check" size={14} />}</span>
    </button>)}
    <div className="flex items-center justify-between gap-3 border-t border-border mt-1 px-2 pt-2 pb-1 text-[13px]">
      <span>按住录音</span>
      <Toggle on={speech.preferences.holdToRecord} ariaLabel="按住录音" onChange={(holdToRecord) => speech.configureInput({ holdToRecord })} />
    </div>
  </AnchoredMenu>;
}

export function SpeechMicrophoneButton({ speech, disabled, onBeforeStart }) {
  const anchor = useRef(null);
  const [open, setOpen] = useState(false);
  useEffect(() => { if (disabled || speech.active || !speech.available) setOpen(false); }, [disabled, speech.active, speech.available]);
  const openMenu = useCallback(() => {
    closeDesktopContextMenu();
    if (!disabled && !speech.active && speech.available) setOpen(true);
  }, [disabled, speech.active, speech.available]);
  useEffect(() => {
    const handler = (event) => {
      const detail = event.detail;
      if (!detail || detail.handled || detail.name !== SPEECH_MICROPHONE_CONTEXT_MENU_DELEGATE
        || detail.element !== anchor.current) return;
      // The global menu owns document capture; claim only this composer's button.
      detail.handled = true;
      openMenu();
    };
    window.addEventListener(CONTEXT_MENU_DELEGATE_EVENT, handler);
    return () => window.removeEventListener(CONTEXT_MENU_DELEGATE_EVENT, handler);
  }, [openMenu]);
  const beginHold = (kind) => { onBeforeStart?.(); speech.beginHold(kind); };
  return <>
    <button ref={anchor} type="button" data-composer-speech="true" aria-label="听写" title="右键配置麦克风"
      data-desktop-context-menu-delegate={SPEECH_MICROPHONE_CONTEXT_MENU_DELEGATE}
      aria-haspopup="menu" aria-expanded={open} disabled={disabled}
      className="w-7 h-7 rounded-full shrink-0 flex items-center justify-center text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50 focus-visible:outline-2 focus-visible:outline-accent"
      onContextMenu={(event) => { event.preventDefault(); event.stopPropagation(); openMenu(); }}
      onPointerDown={(event) => {
        if (event.button === 0 && speech.preferences.holdToRecord) { event.preventDefault(); beginHold({ kind: 'pointer', id: event.pointerId }); }
      }}
      onKeyDown={(event) => {
        if (speech.preferences.holdToRecord && ['Enter', ' '].includes(event.key)) {
          event.preventDefault(); if (!event.repeat) beginHold({ kind: 'key', code: event.code });
        }
      }}
      onClick={(event) => {
        if (speech.preferences.holdToRecord && event.detail !== 0) return;
        onBeforeStart?.(); speech.toggle();
      }}><VsIcon name="microphone" size={17} /></button>
    {open && <MicrophoneMenu speech={speech} anchorRef={anchor} onClose={() => setOpen(false)} />}
  </>;
}
