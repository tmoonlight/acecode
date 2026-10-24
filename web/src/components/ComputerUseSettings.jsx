import { useEffect, useId, useState, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { computerUseSettingsStore } from '../lib/computerUseSettings.js';
import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';

const ARROW_PATH = 'M0 0 L0 26 L7 20 L13 31 L19 28 L13 18 L24 18 Z';
const ACE_GLYPHS = [
  [0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11],
  [0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f],
  [0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f],
];
const ACE_MARK_PATH = ACE_GLYPHS.flatMap((rows, letter) => rows.flatMap((bits, row) =>
  [0, 1, 2, 3, 4].filter((column) => bits & (1 << (4 - column)))
    .map((column) => `M${24 + letter * 6 + column} ${27 + row}h1v1h-1z`))).join('');

// The same hotspot-relative polygon and 5x7 ACE glyphs as the native pointer.
function PointerPreview({ marked }) {
  const clip = useId();
  return <svg viewBox="-18 -18 66 58" className="w-24 h-24 overflow-visible" aria-hidden="true">
    <defs><clipPath id={clip}><path d={ARROW_PATH} /></clipPath></defs>
    <path d={ARROW_PATH} fill="var(--ace-accent)" stroke="var(--ace-computer-pointer-outline)" strokeWidth="2.1" strokeLinejoin="round" />
    <path d={ARROW_PATH} fill="none" stroke="var(--ace-computer-pointer-highlight)" strokeWidth="1.7" clipPath={`url(#${clip})`} />
    {marked && <>
      <rect x="20" y="24" width="25" height="13" rx="3" fill="var(--ace-accent)" stroke="var(--ace-computer-pointer-outline)" strokeWidth="1.5" />
      <rect x="20.65" y="24.65" width="23.7" height="11.7" rx="2.35" fill="none" stroke="var(--ace-computer-pointer-highlight)" strokeWidth="0.65" />
      <path d={ACE_MARK_PATH} fill="var(--ace-computer-pointer-highlight)" />
    </>}
  </svg>;
}

export function ComputerUseSettings({ onCheckUpdates }) {
  const [expanded, setExpanded] = useState(false);
  const configId = useId();
  const store = computerUseSettingsStore(api);
  const { snapshot, loading, saving, error } = useSyncExternalStore(store.subscribe, store.getSnapshot);
  useEffect(() => { void store.load(); }, [store]);
  const supported = snapshot?.supported === true;
  const availability = snapshot?.availability;
  const macos = snapshot?.platform === 'macos';
  useEffect(() => {
    if (!macos) return undefined;
    const refresh = () => { if (document.visibilityState === 'visible') void store.load(); };
    window.addEventListener('focus', refresh);
    return () => window.removeEventListener('focus', refresh);
  }, [macos, store]);
  const errorMessage = error ? lookupErrorMessage(error.code,
    error.action === 'load' ? '加载电脑操控配置失败' : error.action === 'permission' ?
      '暂时无法确认系统授权，请完成授权后刷新状态；必要时重新启动 ACECode' : '保存电脑操控配置失败，请重试确认状态') : '';

  return (
    <div className="bg-surface border border-border mb-2" aria-busy={loading || saving} data-computer-use="true">
      <div className="flex items-center gap-3 px-3.5 py-3 border border-border">
        <div data-settings-surface="true" className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
          <VsIcon name="computer" size={20} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="text-[13px] font-normal">电脑操控（实验性）</div>

          {snapshot && !supported && <div className="text-[11px] text-fg-mute mt-0.5">电脑操控支持 Windows 和 macOS 14 及以上版本</div>}
        </div>
        <button type="button" className="px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50"
          disabled={!snapshot || loading} aria-expanded={expanded} aria-controls={configId}
          onClick={() => setExpanded((value) => !value)}>{expanded ? '收起' : '配置'}</button>
        <Toggle on={supported && snapshot?.enabled === true} disabled={!snapshot || !supported || loading || saving}
          ariaLabel="启用电脑操控" onChange={(enabled) => { void store.setEnabled(enabled); }} />
      </div>
      {macos && supported && <div className="border-t border-border px-3.5 py-3">
        <div className="flex items-center justify-between gap-3 mb-2">
          <div className="text-[13px] font-normal text-fg">系统授权</div>
          <button type="button" disabled={loading || saving} className="px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50"
            onClick={() => { void store.load(); }}>刷新授权状态</button>
        </div>
        {[
          { key: 'accessibility', label: '辅助功能', detail: '读取应用控件并操作鼠标和键盘' },
          { key: 'screen_recording', label: '屏幕录制', detail: '获取目标窗口的截图' },
        ].map((permission) => {
          const granted = availability?.[permission.key] === 'granted';
          return <div key={permission.key} className="flex items-center justify-between gap-3 py-2">
            <div className="min-w-0">
              <div className="text-[13px] font-normal text-fg">{permission.label}</div>
              <div className="text-[11px] text-fg-mute mt-0.5">{permission.detail}</div>
            </div>
            {granted ? <span className="text-[12px] text-ok">已授权</span> :
              <button type="button" disabled={loading || saving || !availability?.helper_available}
                className="px-1.5 py-0.5 text-[11px] text-accent hover:underline disabled:opacity-50"
                onClick={() => { void store.requestPermission(permission.key); }}>前往授权</button>}
          </div>;
        })}
        {availability?.error ? <p role="status" className="text-[11px] text-warn mt-2">
          {availability.error === 'COMPUTER_USE_HELPER_MISSING' ? '电脑操控组件缺失，请重新安装完整的 ACECode 安装包' :
            '暂时无法确认系统授权，请完成授权后刷新状态；必要时重新启动 ACECode'}
        </p> : <p className="text-[11px] text-fg-mute mt-2">
          {availability?.ready ? '系统授权已就绪，开启上方开关后即可使用电脑操控' :
            '请在系统设置中允许本次 ACECode 启动所对应的应用，完成后返回并刷新授权状态'}
        </p>}
      </div>}
      {error && <div role="alert" className="px-3.5 pb-3 text-[12px] text-danger">
        {errorMessage}
        {error.code === 'COMPUTER_USE_SETTINGS_UNSUPPORTED' && onCheckUpdates &&
          <button type="button" onClick={onCheckUpdates} className="ml-2 text-accent hover:underline">检查更新</button>}
        <button type="button" disabled={loading || saving} className="ml-2 hover:underline"
          onClick={() => { void store.retry(); }}>重试</button>
      </div>}
      {expanded && <div id={configId} className="border-t border-border px-3.5 py-3">
        <div className="text-[13px] font-normal mb-1">指针样式</div>

        <div role="radiogroup" aria-label="指针样式" className="grid grid-cols-2 gap-3 max-w-md">
          {[
            { value: 'ace', label: 'ACE 标记' },
            { value: 'plain', label: '纯色指针' },
          ].map((option) => {
            const selected = snapshot?.pointer_style === option.value;
            return <label key={option.value} data-settings-surface="true"
              className={`relative min-w-0 p-3 rounded-lg border cursor-pointer focus-within:ring-2 focus-within:ring-accent focus-within:ring-offset-2 ${selected ? 'border-accent bg-accent-bg' : 'border-border bg-surface hover:border-accent'} ${!snapshot || loading ? 'opacity-50' : ''}`}>
              <input type="radio" name={`${configId}-pointer-style`} value={option.value} checked={selected}
                disabled={!snapshot || loading} className="sr-only"
                onChange={() => { void store.setPointerStyle(option.value); }} />
              <div className="h-24 flex items-center justify-center"><PointerPreview marked={option.value === 'ace'} /></div>
              <div className="text-[13px] font-normal text-fg">{option.label}</div>
              {selected && <span aria-hidden="true" className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
            </label>;
          })}
        </div>
      </div>}
    </div>
  );
}
