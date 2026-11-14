import { useEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { SHARE_SESSION_EVENT, INVITE_LINK_EVENT } from '../lib/sharingEvents.js';
import { decodeInviteLink, inviteErrorMessage } from '../lib/inviteLink.js';
import { Modal } from './Modal.jsx';
import { ShareDialogHeader, ShareSessionDialog } from './ShareSessionDialog.jsx';
import { RemoteSessionView } from './RemoteSessionView.jsx';

export function SharingHost() {
  const [target, setTarget] = useState(null);
  const [invite, setInvite] = useState(null);
  const [peer, setPeer] = useState(null);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState('');
  const pendingInvites = useRef([]);
  const activeInvite = useRef(null);
  useEffect(() => {
    window.__aceInviteReceiverReady = true;
    const share = (event) => setTarget(event.detail || {});
    const open = (event) => {
      if (!event.detail || typeof event.detail.link !== 'string') return;
      if (activeInvite.current) { pendingInvites.current.push(event.detail); return; }
      activeInvite.current = event.detail; setInvite(event.detail); setError('');
    };
    const native = (event) => open({ detail: { link: event.detail?.link || event.detail } });
    window.addEventListener(SHARE_SESSION_EVENT, share);
    window.addEventListener(INVITE_LINK_EVENT, open);
    window.addEventListener('acecode:open-invite', native);
    for (const link of (window.__acePendingInvites || []).splice(0)) native({ detail: link });
    return () => { window.__aceInviteReceiverReady = false; window.removeEventListener(SHARE_SESSION_EVENT, share); window.removeEventListener(INVITE_LINK_EVENT, open); window.removeEventListener('acecode:open-invite', native); };
  }, []);
  const parsed = invite ? decodeInviteLink(invite.link) : null;
  const advance = () => { activeInvite.current = pendingInvites.current.shift() || null; setInvite(activeInvite.current); setError(''); };
  const cancel = () => { if (busy) return; const callback = invite?.onCancel; advance(); callback?.(); };
  return <>
    {target && <ShareSessionDialog api={target.api || api} sessionId={target.sessionId} title={target.title} onClose={() => setTarget(null)} onOpenPeer={(value) => { setPeer(value); setTarget(null); }} />}
    {peer && <RemoteSessionView api={api} peer={peer} onClose={() => setPeer(null)} />}
    {invite && <Modal key={invite.link} onClose={cancel} dismissOnBackdrop={false} dismissOnEscape={!busy} width={440} labelledBy="invite-connect-title">
      <div className="p-5">
        <ShareDialogHeader title="连接会话" onClose={cancel} id="invite-connect-title" />
        {parsed.value && <div className="space-y-2 mb-5 text-[13px]"><div className="break-words">{parsed.value.owner}</div><div className="break-words">{parsed.value.title}</div><div className="text-fg-mute break-all">{parsed.value.addresses.join(', ')}:{parsed.value.port}</div></div>}
        {(error || parsed.error) && <div role="alert" className="text-danger text-[13px] mb-4">{error || inviteErrorMessage(parsed.error)}</div>}
        <div className="flex justify-end gap-2">
          <button type="button" className="px-3 py-1.5 text-[13px] rounded-md hover:bg-surface-hi disabled:opacity-50" disabled={busy} onClick={cancel}>取消</button>
          <button type="button" data-ace-dialog-primary disabled={busy || !parsed.value} className="px-3 py-1.5 text-[13px] rounded-md bg-accent text-white disabled:opacity-50"
            onClick={async () => { setBusy(true); setError(''); try { setPeer(await api.connectPeer(invite.link)); advance(); } catch (e) { setError(inviteErrorMessage(e.code)); } finally { setBusy(false); } }}>{busy ? '正在连接' : '连接'}</button>
        </div>
      </div>
    </Modal>}
  </>;
}
