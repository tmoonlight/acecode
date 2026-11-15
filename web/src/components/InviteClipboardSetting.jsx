import { useEffect, useSyncExternalStore } from 'react';
import { inviteClipboardStore } from '../lib/inviteClipboardStore.js';

export function InviteClipboardSetting() {
  const available = typeof window.aceDesktop_getInviteClipboardPreference === 'function';
  const state = useSyncExternalStore(inviteClipboardStore.subscribe, inviteClipboardStore.getSnapshot);
  useEffect(() => { if (available) void inviteClipboardStore.load(); }, [available]);
  if (!available) return null;
  return <div className="mb-4">
    <label className="flex items-center justify-between px-3.5 py-2.5 rounded-md bg-surface border border-border text-[13px]">
      识别剪贴板中的会话邀请
      <input type="checkbox" role="switch" checked={state.enabled} disabled={!state.loaded} onChange={(event) => void inviteClipboardStore.set(event.target.checked)} className="accent-accent" />
    </label>
    {state.error && <div role="alert" className="text-[12px] text-danger mt-2">{state.error}<button type="button" className="ml-2 underline" disabled={state.saving} onClick={() => void (state.loaded ? inviteClipboardStore.retry() : inviteClipboardStore.load())}>重试</button></div>}
  </div>;
}
