import { useEffect, useState } from 'react';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { copyTextToClipboard } from '../lib/codeBlockCopy.js';
import { inviteErrorMessage } from '../lib/inviteLink.js';

const fieldClass = 'h-8 px-2 rounded-md border border-border bg-surface-alt text-fg text-[13px] outline-none focus:border-accent';
const buttonClass = 'px-3 py-1.5 rounded-md border border-border text-[13px] hover:bg-surface-hi disabled:opacity-50';

export function ShareDialogHeader({ title, onClose, id = 'share-dialog-title' }) {
  return <div className="flex items-center justify-between gap-3 mb-5">
    <h2 className="text-[16px] font-semibold min-w-0 truncate" id={id}>{title}</h2>
    <button type="button" onClick={onClose} aria-label="关闭" className="w-7 h-7 rounded-md flex items-center justify-center hover:bg-surface-hi"><VsIcon name="close" size={16} /></button>
  </div>;
}

export function ShareSessionDialog({ api, sessionId, title, onClose, onOpenPeer }) {
  const [mode, setMode] = useState('interactive');
  const [visibility, setVisibility] = useState('conversation');
  const [expiresIn, setExpiresIn] = useState(86400);
  const [shares, setShares] = useState([]);
  const [peers, setPeers] = useState([]);
  const [generated, setGenerated] = useState(null);
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);
  const [copied, setCopied] = useState(false);
  const [asks, setAsks] = useState(null);
  const [manage, setManage] = useState(!sessionId);
  const reload = async () => {
    const [shared, connected] = await Promise.all([api.listShares(), api.listPeers()]);
    setShares(shared.shares || []); setPeers(connected.peers || []);
  };
  useEffect(() => { let active = true;
    Promise.all([api.listShares(), api.listPeers()]).then(([shared, connected]) => {
      if (active) { setShares(shared.shares || []); setPeers(connected.peers || []); }
    }).catch((e) => { if (active) setError(inviteErrorMessage(e.code)); });
    return () => { active = false; };
  }, [api]);
  const perform = async (action) => {
    setError(''); setBusy(true);
    try { await action(); } catch (e) { setError(inviteErrorMessage(e.code)); } finally { setBusy(false); }
  };
  const copy = (share, invitation) => perform(async () => {
    await window.aceDesktop_markInviteSeen?.(share.link);
    await copyTextToClipboard(invitation ? `${share.owner} 邀请你查看会话「${share.title}」，请用 ACECode 打开：${share.link}` : share.link);
    setCopied(true);
  });
  return <Modal onClose={onClose} dismissOnBackdrop={false} width={540} labelledBy="share-dialog-title">
    <div className="p-5 max-h-[85vh] overflow-y-auto">
      <ShareDialogHeader title={manage ? '分享与连接' : '分享会话'} onClose={onClose} />
      {!manage && <>
        <div className="truncate text-[13px] text-fg mb-4">{title}</div>
        <div className="grid grid-cols-2 gap-2 mb-4" role="radiogroup" aria-label="链接权限">
          {[['read_only', '只读'], ['interactive', '可交互']].map(([value, label]) => <button key={value} type="button" role="radio" aria-checked={mode === value}
            disabled={busy}
            onClick={() => { setMode(value); setGenerated(null); setCopied(false); }}
            className={`${buttonClass} ${mode === value ? 'border-accent bg-accent-bg text-accent' : 'bg-surface'}`}>{label}</button>)}
        </div>
        <div className="flex flex-wrap gap-4 mb-4">
          <label className="flex items-center gap-2 text-[13px]">可见内容<select disabled={busy} className={fieldClass} value={visibility} onChange={(e) => { setVisibility(e.target.value); setGenerated(null); }}>
            <option value="conversation">只看对话</option><option value="tools">含工具输出</option>
          </select></label>
          <label className="flex items-center gap-2 text-[13px]">有效期<select disabled={busy} className={fieldClass} value={expiresIn} onChange={(e) => { setExpiresIn(Number(e.target.value)); setGenerated(null); }}>
            <option value={3600}>1 小时</option><option value={86400}>1 天</option><option value={604800}>7 天</option>
          </select></label>
        </div>
        <p className="text-[12px] text-fg-mute mb-4">{mode === 'interactive' ? '持有链接即可访问，仅限可信局域网。提问消耗你的模型额度。' : '仅限可信局域网。持有链接的人可以查看此会话。'}</p>
        {generated ? <div className="flex flex-col gap-3">
          <input readOnly aria-label="邀请链接" value={generated.link} className={`${fieldClass} w-full`} onFocus={(e) => e.target.select()} />
          <div className="flex items-center gap-2">
            <button type="button" className="px-3 py-1.5 rounded-md bg-accent text-white text-[13px] disabled:opacity-50" disabled={busy} onClick={() => copy(generated, true)}>{copied ? '已复制' : '复制邀请'}</button>
            <button type="button" className={buttonClass} disabled={busy} onClick={() => copy(generated, false)}>复制链接</button>
          </div>
        </div> : <button type="button" data-ace-dialog-primary className="px-3 py-1.5 rounded-md bg-accent text-white text-[13px] disabled:opacity-50" disabled={busy}
          onClick={() => perform(async () => { setGenerated(await api.createShare(sessionId, { mode, visibility, expires_in: expiresIn })); await reload(); window.dispatchEvent(new Event('acecode:shares-changed')); })}>{busy ? '正在生成' : '生成链接'}</button>}
        <div className="h-px bg-border my-5" />
        <button type="button" className="text-[13px] text-accent hover:underline" onClick={() => setManage(true)}>分享与连接</button>
      </>}
      {manage && <div className="flex flex-col gap-5">
        <section><h3 className="text-[13px] mb-2">我的分享</h3>
          {!shares.some((share) => share.active) && <div className="text-[13px] text-fg-mute">暂无分享</div>}
          {shares.filter((share) => share.active).map((share) => <div key={share.id} className="flex items-center gap-2 py-2 border-b border-border">
            <div className="min-w-0 flex-1"><div className="text-[13px] truncate">{share.title}</div><span className="text-[12px] text-fg-mute">{share.allow_questions ? '可交互' : '只读'}</span></div>
            <button type="button" className={buttonClass} disabled={busy || !share.link} onClick={() => copy(share, true)}>复制</button>
            <button type="button" className={buttonClass} disabled={busy} onClick={() => perform(async () => setAsks((await api.shareQuestions(share.id)).asks || []))}>提问记录</button>
            <button type="button" className={`${buttonClass} text-danger`} disabled={busy} onClick={() => perform(async () => { await api.revokeShare(share.id); await reload(); window.dispatchEvent(new Event('acecode:shares-changed')); })}>撤销</button>
          </div>)}
        </section>
        <section><h3 className="text-[13px] mb-2">已连接的会话</h3>
          {!peers.length && <div className="text-[13px] text-fg-mute">暂无连接</div>}
          {peers.map((peer) => <div key={peer.id} className="flex items-center gap-2 py-2 border-b border-border">
            <button type="button" className="text-left min-w-0 flex-1" onClick={() => onOpenPeer(peer)}><span className="block text-[13px] truncate">{peer.title}</span><span className="text-[12px] text-fg-mute">{peer.owner}{peer.status === 'expired' || peer.status === 'invalid' ? ' · 已失效' : ''}</span></button>
            <button type="button" className={`${buttonClass} text-danger`} disabled={busy} onClick={() => perform(async () => { await api.deletePeer(peer.id); await reload(); })}>删除</button>
          </div>)}
        </section>
        {asks && <section><h3 className="text-[13px] mb-2">提问记录</h3>{!asks.length && <div className="text-[13px] text-fg-mute">暂无提问</div>}{asks.map((ask, index) => <div key={index} className="text-[13px] py-3 border-b border-border"><div className="text-[12px] text-fg-mute mb-1">{ask.name} · {new Date(ask.time * 1000).toLocaleString()}</div><div className="whitespace-pre-wrap">{ask.question}</div><div className="whitespace-pre-wrap mt-2">{ask.answer}</div></div>)}</section>}
      </div>}
      {error && <div role="alert" className="text-[13px] text-danger mt-3">{error}</div>}
    </div>
  </Modal>;
}
