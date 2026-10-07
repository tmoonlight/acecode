import { useEffect, useRef, useState } from 'react';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { buildOfficePreviewDocument } from '../lib/desktopOfficePreview.js';

export function VirtualOfficeWelcome({ office, onClose }) {
  const [preview, setPreview] = useState('');
  const [previewFailed, setPreviewFailed] = useState(false);
  const [paused, setPaused] = useState(() => window.matchMedia('(prefers-reduced-motion: reduce)').matches);
  const frame = useRef(null);
  useEffect(() => {
    let disposed = false;
    Promise.resolve().then(() => window.aceDesktop_getOfficePreview()).then(html => {
      if (disposed) return;
      if (typeof html !== 'string' || !html.includes('<head>')) throw new Error('Missing preview');
      setPreview(buildOfficePreviewDocument(html));
    }).catch(() => { if (!disposed) setPreviewFailed(true); });
    return () => { disposed = true; };
  }, []);
  useEffect(() => {
    const motion = window.matchMedia('(prefers-reduced-motion: reduce)');
    const change = () => setPaused(motion.matches);
    motion.addEventListener('change', change);
    return () => motion.removeEventListener('change', change);
  }, []);
  const syncPause = () => frame.current?.contentWindow?.postMessage({ type: 'ace-office-preview-pause', paused }, '*');
  useEffect(syncPause, [paused, preview]);
  const close = () => { if (!office.busy) onClose(); };
  const enable = async () => { const result = await office.setEnabled(true); if (result.ok) onClose(); };
  return (
    <Modal width={480} onClose={close} dismissOnBackdrop={false} labelledBy="office-welcome-title">
      <div className="p-5">
        <div className="flex items-center justify-between gap-3">
          <h2 id="office-welcome-title" className="text-xl font-bold">开启虚拟办公室？</h2>
          <button type="button" aria-label="关闭" disabled={office.busy} onClick={close}
            className="w-8 h-8 shrink-0 flex items-center justify-center rounded-md text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-60">
            <VsIcon name="close" size={18} />
          </button>
        </div>
        <p className="text-[13px] text-fg-2 mt-3">打开虚拟办公室，为您的工作增添更多乐趣！</p>
        <div className="relative mt-4 bg-surface-alt rounded-md overflow-hidden" style={{ aspectRatio: '344 / 252' }}>
          {preview ? <iframe ref={frame} srcDoc={preview} onLoad={syncPause} sandbox="allow-scripts"
            title="虚拟办公室预览" aria-hidden="true" tabIndex={-1}
            className="w-full h-full border-0 pointer-events-none" />
            : <div role="status" className="absolute inset-0 flex items-center justify-center text-[12px] text-fg-mute">
                {previewFailed ? '预览暂时不可用，仍可开启虚拟办公室。' : '正在加载预览…'}
              </div>}
        </div>
        <div className="flex items-center justify-between gap-3 mt-2 text-[11px] text-fg-mute">
          <span>演示画面，开启后将跟随您的会话。</span>
          {preview && <button type="button" onClick={() => setPaused(value => !value)} className="shrink-0 px-1.5 py-1 hover:text-fg">
            {paused ? '播放动画' : '暂停动画'}
          </button>}
        </div>
        {office.error && <p role="alert" className="text-[12px] text-danger mt-3">{office.error}</p>}
        <div className="flex justify-end gap-2 mt-5 text-[13px]">
          <button type="button" onClick={close} disabled={office.busy}
            className="px-3 py-1.5 rounded hover:bg-surface-hi disabled:opacity-60">暂不开启</button>
          <button type="button" onClick={enable} disabled={office.busy} data-ace-dialog-primary
            className="px-3 py-1.5 bg-accent text-white rounded disabled:opacity-60">开启虚拟办公室</button>
        </div>
      </div>
    </Modal>
  );
}
