import { useEffect, useSyncExternalStore } from 'react';
import { phoneRequest } from '../lib/androidPhone.js';
import { createPhoneSettingsStore } from '../lib/androidPhoneSettings.js';
import { VsIcon } from './Icon.jsx';
import { Toggle } from './Modal.jsx';
import clippyPreview from '../../../assets/desktop_pet/classic/clippy/preview.png';
import roverPreview from '../../../assets/desktop_pet/classic/rover/preview.png';
import whalePreview from '../../../assets/desktop_pet/whale/preview.png';

const settings = createPhoneSettingsStore(phoneRequest);
const PETS = [
  { id: 'off', label: '关闭' },
  { id: 'whale', label: '鲸鱼娘', preview: whalePreview },
  { id: 'clippy', label: '大眼回形针', preview: clippyPreview },
  { id: 'rover', label: 'Rover', preview: roverPreview },
];

function PhoneIcon() {
  return <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round" strokeLinejoin="round" aria-hidden="true">
    <rect x="6" y="2" width="12" height="20" rx="2.5" /><path d="M10 5h4M11 19h2" />
  </svg>;
}

export function AndroidPhoneSettings({ pet = false }) {
  const { status, pending, error, draftPet } = useSyncExternalStore(settings.subscribe, settings.getSnapshot);
  const available = !!globalThis.window?.acePhone;
  useEffect(() => {
    if (!available) return undefined;
    const refresh = () => { void settings.refresh(); };
    refresh();
    window.addEventListener('ace-phone-resume', refresh);
    return () => window.removeEventListener('ace-phone-resume', refresh);
  }, [available]);
  if (!available) return null;
  const busy = pending > 0;
  const selected = draftPet ?? status?.pet ?? 'off';
  const needsOverlay = !!status && !status.overlay && selected !== 'off';
  return (
    <section className="mb-2 border border-border bg-surface" data-android-phone-settings={pet ? 'pet' : 'control'} aria-busy={busy}>
      <div className="flex items-center gap-3 px-3.5 py-3">
        <div data-settings-surface="true" className="w-10 h-10 shrink-0 rounded-md border border-border bg-surface-alt flex items-center justify-center text-fg">
          {pet ? <VsIcon name="sparkle" size={20} /> : <PhoneIcon />}
        </div>
        <div className="min-w-0 flex-1">
          <div className="text-[13px] font-normal text-fg">{pet ? '悬浮宠物' : '手机操控'}</div>
          {(!pet || busy || !status || needsOverlay) && <div className="mt-0.5 flex items-center gap-1.5 text-[11px] text-fg-mute" role="status">
            {busy ? <><span className="ace-spinner w-3 h-3" />正在更新</> : !status ? '状态未加载' : needsOverlay ? '需要悬浮窗权限'
              : <><span className={`w-1.5 h-1.5 rounded-full ${status.accessibility ? 'bg-ok' : 'bg-fg-mute'}`} />{status.accessibility ? '已开启' : '未开启'}</>}
          </div>}
        </div>
        {!pet && <Toggle ariaLabel="手机操控" on={!!status?.accessibility} disabled={!status || busy}
          onChange={() => { void settings.accessibility(); }} />}
      </div>
      {pet && <div className="grid grid-cols-2 gap-2 px-3.5 pb-3.5 sm:grid-cols-4" role="radiogroup" aria-label="选择悬浮宠物">
        {PETS.map(item => <button key={item.id} type="button" role="radio" aria-checked={selected === item.id}
          disabled={!status || busy} onClick={() => { void settings.setPet(item.id); }}
          className={`relative min-w-0 rounded-lg border p-3 text-left transition focus-visible:outline-2 focus-visible:outline-accent focus-visible:outline-offset-2 disabled:opacity-60 ${selected === item.id ? 'border-accent bg-accent-bg' : 'border-border bg-surface hover:bg-surface-hi'}`}>
          <span className="mb-2 flex h-16 items-center justify-center" aria-hidden="true">
            {item.preview ? <img src={item.preview} alt="" className="max-h-full max-w-full object-contain" /> : <VsIcon name="stop" size={28} className="text-fg-mute" />}
          </span>
          <span className="block text-[13px] font-normal text-fg">{item.label}</span>
          {selected === item.id && <span className="absolute top-2 right-2 w-2 h-2 rounded-full bg-accent" />}
        </button>)}
      </div>}
      {needsOverlay && <div className="flex flex-wrap items-center gap-2 border-t border-border px-3.5 py-2">
        <span className="min-w-0 flex-1 text-[12px] text-fg-mute">允许显示在其他应用上层后生效</span>
        <button type="button" disabled={busy} onClick={() => { void settings.setPet(selected); }}
          className="min-h-11 px-2 text-[12px] text-accent disabled:opacity-50">前往授权</button>
      </div>}
      {error && <div role="alert" className="flex flex-wrap items-center gap-2 border-t border-border px-3.5 py-2 text-[12px] text-danger">
        <span className="min-w-0 flex-1 break-words">{error}</span>
        <button type="button" disabled={busy} className="min-h-11 px-2 disabled:opacity-50" onClick={() => { void settings.retry(); }}>重试</button>
      </div>}
    </section>
  );
}
