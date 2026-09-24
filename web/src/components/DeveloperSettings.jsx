import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { startFullscreenHeatWave } from '../lib/fullscreenHeatWave.js';
import { applyJbModeTransition, initialJbMode } from '../lib/jbMode.js';
import { Toggle } from './Modal.jsx';
import { ToolPreambleSettings } from './ToolPreambleSettings.jsx';

export function DeveloperSettings({ onThemeChange }) {
  const [enabled, setEnabled] = useState(false);
  const [loaded, setLoaded] = useState(false);
  const [busy, setBusy] = useState(false);
  const [jbEnabled, setJbEnabled] = useState(false);
  const [jbLoaded, setJbLoaded] = useState(false);
  const [jbBusy, setJbBusy] = useState(false);
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
    api.getJbMode()
      .then((state) => {
        if (cancelled) return;
        setJbEnabled(initialJbMode(state.enabled));
        setJbLoaded(true);
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

  const saveJb = async (next) => {
    if (!jbLoaded || jbBusy) return;
    setJbBusy(true);
    setError('');
    try {
      const value = await applyJbModeTransition({
        previous: jbEnabled,
        next,
        persist: async (enabled) => {
          const state = await api.setJbMode(enabled);
          return state.enabled === true;
        },
        setTheme: (theme) => onThemeChange?.(theme),
        startHeatWave: startFullscreenHeatWave,
      });
      setJbEnabled(value);
    } catch {
      setError('保存开发者配置失败，请重试');
    } finally {
      setJbBusy(false);
    }
  };

  return (
    <div>
      <h2 className="text-xl font-bold mb-5">开发者模式</h2>
      <div className="text-[14px] font-semibold mb-1">桌面启动</div>
      <p className="text-[12px] text-fg-mute mb-3">用于同时运行多个 ACECode 桌面实例，方便开发和调试 ACECode。</p>
      <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-normal text-fg">允许多进程启动</div>
          <div className="text-[11px] text-fg-mute mt-0.5">全局生效。开启后，新启动的桌面实例可与已有实例同时运行；关闭后不影响已打开的实例。</div>
        </div>
        <Toggle on={enabled} onChange={save} disabled={!loaded || busy} ariaLabel="允许多进程启动" />
      </div>
      <div className="flex items-center justify-between gap-4 px-3.5 py-2.5 rounded-md bg-surface border border-border mb-2">
        <div>
          <div className="text-[13px] font-normal text-fg">打开JB模式</div>
          <div className="text-[11px] text-fg-mute mt-0.5">打开后去掉系统提示词里的拒绝和停顿说明，并切换到暗黑模式。</div>
        </div>
        <Toggle on={jbEnabled} onChange={saveJb} disabled={!jbLoaded || jbBusy} ariaLabel="打开JB模式" />
      </div>
      {error && (
        <div role="alert" className="text-[12px] text-danger mt-2">
          {error}
          <button type="button" disabled={busy} onClick={() => setReload((value) => value + 1)}
            className="ml-2 underline disabled:opacity-50">重试</button>
        </div>
      )}
      <div className="h-px bg-border my-5" />
      <ToolPreambleSettings />
    </div>
  );
}
