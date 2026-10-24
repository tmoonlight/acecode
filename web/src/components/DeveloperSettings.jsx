import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { Toggle } from './Modal.jsx';

export function DeveloperSettings() {
  const [enabled, setEnabled] = useState(false);
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const [reload, setReload] = useState(0);

  useEffect(() => {
    let cancelled = false;
    setLoaded(false);
    setError('');
    api.getDesktopMultiInstance()
      .then((state) => {
        if (cancelled) return;
        setEnabled(state.enabled === true);
        setLoaded(true);
      })
      .catch(() => {
        if (!cancelled) setError('加载开发者配置失败');
      });
    return () => { cancelled = true; };
  }, [reload]);

  const save = async (next) => {
    if (!loaded || busy) return;
    setBusy(true);
    setError('');
    try {
      const state = await api.setDesktopMultiInstance(next);
      setEnabled(state.enabled === true);
    } catch {
      setError('保存开发者配置失败，请重试');
    } finally {
      setBusy(false);
    }
  };

  return (
    <div>
      <h2 className="text-xl font-bold mb-5">开发者模式</h2>
      <div className="text-[14px] font-semibold mb-1">桌面启动</div>

      <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-normal text-fg">允许多进程启动</div>
          <div className="text-[11px] text-fg-mute mt-0.5">仅影响新启动的实例。</div>
        </div>
        <Toggle on={enabled} onChange={save} disabled={!loaded || busy} ariaLabel="允许多进程启动" />
      </div>
      {error && (
        <div role="alert" className="text-[12px] text-danger mt-2">
          {error}
          <button type="button" disabled={busy} onClick={() => setReload((value) => value + 1)}
            className="ml-2 underline disabled:opacity-50">重试</button>
        </div>
      )}
    </div>
  );
}
