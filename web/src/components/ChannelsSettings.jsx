// 消息通道(设置 > 集成 > 消息通道,openspec add-desktop-im-channels / add-more-im-channels)。
// 页面只有两列平台卡片:「连接」在还没配好时打开分步向导,配好的直接连接;连上后按钮变成
// 「取消连接」。卡片只显示一行简要状态和待批准请求;连接原因、隐私模式、收发计数等细节由 daemon
// 写进日志,不在页面展开(2026-10-05 验收反馈)。各平台向导的说明、字段与机主绑定方式在
// lib/channelsSettings.js 的 platformSetup;状态变化经 WS(channels_*)实时合并。
import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { connection } from '../lib/connection.js';
import { clsx } from '../lib/format.js';
import {
  applyChannelsEvent,
  bindView,
  channelCardView,
  channelCards,
  channelErrorText,
  connectNeedsWizard,
  connectStepView,
  contactRows,
  credentialFields,
  credentialPlaceholder,
  credentialsPayload,
  discordInviteUrl,
  initialWizardStep,
  normalizeBind,
  normalizeChannelsSnapshot,
  ownerStepView,
  pendingRows,
  platformSetup,
  qrSvgModel,
  savedBotSummary,
} from '../lib/channelsSettings.js';
import { copyTextToSystemClipboard } from '../lib/systemClipboard.js';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const inputClass = 'w-full h-8 px-2.5 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-50';
const primaryButtonClass = 'px-3 py-1 text-[12px] bg-accent text-white rounded disabled:opacity-60 shrink-0';
const secondaryButtonClass = 'px-3 py-1 text-[12px] rounded border border-border bg-surface hover:bg-surface-hi transition disabled:opacity-50 shrink-0';
const dangerButtonClass = 'px-3 py-1 text-[12px] bg-danger text-white rounded disabled:opacity-60 shrink-0';
const linkButtonClass = 'px-1 py-0.5 text-[11px] text-fg-mute hover:text-fg hover:underline disabled:opacity-50';
const iconButtonClass = 'w-7 h-7 inline-flex items-center justify-center rounded text-fg-mute hover:text-fg hover:bg-surface-alt transition';

const TONE_TEXT = { ok: 'text-ok', danger: 'text-danger', warn: 'text-warn', mute: 'text-fg-mute' };
const TONE_DOT = {
  ok: 'bg-ok shadow-[0_0_4px_var(--ace-ok)]',
  danger: 'bg-danger shadow-[0_0_4px_var(--ace-danger)]',
  warn: 'bg-warn shadow-[0_0_4px_var(--ace-warn)]',
  mute: 'bg-fg-mute',
};

function StatusPill({ tone = 'mute', text }) {
  return (
    <span className={clsx('flex items-center gap-1.5 text-[12px] min-w-0', TONE_TEXT[tone] || TONE_TEXT.mute)}>
      <span className={clsx('w-2 h-2 rounded-full shrink-0', TONE_DOT[tone] || TONE_DOT.mute)} />
      <span className="truncate">{text}</span>
    </span>
  );
}

function PlatformIcon({ name, size = 18, box = 'h-9 w-9' }) {
  return (
    <span className={clsx('flex shrink-0 items-center justify-center rounded-md border border-border bg-surface-alt text-fg-2', box)}>
      <VsIcon name={name} size={size} />
    </span>
  );
}

// 二维码在亮/暗主题下都保持白底黑块:反色二维码不是所有手机都能扫。
function QrCode({ value, label }) {
  const model = useMemo(() => qrSvgModel(value), [value]);
  if (!model) return null;
  return (
    <div data-settings-surface="true" className="inline-flex shrink-0 rounded-md border border-border bg-white p-1">
      <svg role="img" aria-label={label} viewBox={`0 0 ${model.size} ${model.size}`} width="168" height="168"
        shapeRendering="crispEdges">
        <path d={model.path} fill="black" />
      </svg>
    </div>
  );
}

function QrPlaceholder() {
  return (
    <div className="flex h-[176px] w-[176px] shrink-0 items-center justify-center rounded-md bg-surface-alt">
      <span className="ace-spinner" />
    </div>
  );
}

function PendingList({ requests, busy, onDecide }) {
  return requests.map((request) => (
    <div key={request.id} className="flex items-center justify-between gap-2 py-1.5">
      <div className="min-w-0">
        <div className="text-[12px] truncate">{request.title}</div>
        <div className="text-[11px] text-fg-mute truncate">{request.subtitle}</div>
      </div>
      <div className="flex items-center gap-1.5 shrink-0">
        <button type="button" className={primaryButtonClass} disabled={!!busy}
          onClick={() => onDecide(request, true)}>批准</button>
        <button type="button" className={secondaryButtonClass} disabled={!!busy}
          onClick={() => onDecide(request, false)}>拒绝</button>
      </div>
    </div>
  ));
}

function ChannelCard({ card, platform, busy, onConnect, onDisconnect, onFix, onManage, onDecide }) {
  const view = channelCardView(platform, busy);
  const pending = pendingRows(platform);
  const connect = view.button.action === 'connect';
  return (
    <article data-channel-card={card.platform} className="flex min-w-0 flex-col rounded-lg border border-border bg-surface p-4">
      <div className="flex items-center gap-3">
        <PlatformIcon name={card.icon} />
        <div className="min-w-0 flex-1 truncate text-[14px] font-semibold">{card.title}</div>
        <button type="button" disabled={view.button.disabled}
          className={connect ? primaryButtonClass : secondaryButtonClass}
          onClick={connect ? onConnect : onDisconnect}>
          {view.button.label}
        </button>
      </div>
      <p className="mt-3 text-[12px] leading-5 text-fg-mute">{card.description}</p>
      {(view.status || platform.configured) && (
        <div className="mt-3 flex min-h-[22px] items-center justify-between gap-2">
          {view.status ? <StatusPill tone={view.status.tone} text={view.status.text} /> : <span />}
          <span className="flex items-center gap-1 shrink-0">
            {view.status?.action === 'fix' && (
              <button type="button" className={linkButtonClass} onClick={onFix}>处理</button>
            )}
            {platform.configured && (
              <button type="button" className={linkButtonClass} onClick={onManage}>管理</button>
            )}
          </span>
        </div>
      )}
      {pending.length > 0 && (
        <div className="mt-3 border-t border-border pt-2">
          <div className="text-[12px] font-medium text-warn">{`${pending.length} 个请求待批准`}</div>
          <PendingList requests={pending} busy={busy} onDecide={onDecide} />
        </div>
      )}
    </article>
  );
}

function Stepper({ steps, current }) {
  return (
    <ol className="mb-5 flex items-center gap-2">
      {steps.map((label, index) => {
        const done = index < current;
        const active = index === current;
        return (
          <li key={label} className={clsx('flex min-w-0 items-center gap-2', index < steps.length - 1 && 'flex-1')}
            aria-current={active ? 'step' : undefined}>
            <span className={clsx('flex h-6 w-6 shrink-0 items-center justify-center rounded-full border text-[12px]',
              active ? 'border-accent bg-accent text-white' : done ? 'border-accent text-accent' : 'border-border text-fg-mute')}>
              {done ? <VsIcon name="check" size={12} /> : index + 1}
            </span>
            <span className={clsx('truncate text-[12px]', active ? 'font-medium text-fg' : 'text-fg-mute')}>{label}</span>
            {index < steps.length - 1 && <span className="h-px min-w-[12px] flex-1 bg-border" />}
          </li>
        );
      })}
    </ol>
  );
}

function WizardFooter({ children }) {
  return <div className="mt-5 flex items-center justify-end gap-2">{children}</div>;
}

function SavedBot({ title, desc, children }) {
  return (
    <div className="flex items-center justify-between gap-3 rounded-md bg-surface-alt px-3 py-2.5">
      <div className="min-w-0">
        <div className="text-[13px] truncate">{title}</div>
        {desc && <div className="text-[11px] text-fg-mute truncate">{desc}</div>}
      </div>
      <div className="flex items-center gap-1 shrink-0">{children}</div>
    </div>
  );
}

function InlineError({ text }) {
  return text ? <p className="mt-2 text-[12px] text-danger">{text}</p> : null;
}

function Instructions({ items }) {
  if (!items?.length) return null;
  return (
    <ol className="list-decimal space-y-1 pl-5 text-[12px] leading-5 text-fg-2">
      {items.map((item) => <li key={item}>{item}</li>)}
    </ol>
  );
}

function ExternalLinks({ links }) {
  if (!links?.length) return null;
  return (
    <div className="mt-2 flex flex-wrap gap-x-3 gap-y-1">
      {links.map((link) => (
        <a key={link.href} href={link.href} target="_blank" rel="noreferrer"
          className="text-[12px] text-accent hover:underline">{link.label}</a>
      ))}
    </div>
  );
}

// 第一步(填写凭据):说明 + 字段;已经保存过的机器人默认显示摘要,点「更换」再填。
function CredentialFields({ platformKey, platform, draft, setDraft, autoFocus }) {
  return credentialFields(platformKey).map((field, index) => (
    field.kind === 'select' ? (
      <label key={field.key} className="mt-2 flex items-center gap-2 text-[12px]">
        <span className="w-14 shrink-0 text-fg-mute">{field.label}</span>
        <select className={inputClass} value={draft[field.key] ?? field.defaultValue ?? ''}
          onChange={(e) => setDraft((d) => ({ ...d, [field.key]: e.target.value }))} aria-label={field.label}>
          {field.options.map((option) => <option key={option.value} value={option.value}>{option.label}</option>)}
        </select>
      </label>
    ) : (
      <input key={field.key} className={clsx(inputClass, 'mt-2')} type={field.secret ? 'password' : 'text'}
        value={draft[field.key] ?? ''} autoFocus={autoFocus && index === 0}
        placeholder={credentialPlaceholder(platform, field)}
        onChange={(e) => setDraft((d) => ({ ...d, [field.key]: e.target.value }))}
        autoComplete={field.secret ? 'new-password' : 'off'} spellCheck={false} aria-label={field.label} />
    )
  ));
}

function FormCredentialsStep({ platformKey, platform, busy, run, change, onNext, onCancel }) {
  const setup = platformSetup(platformKey);
  const [editing, setEditing] = useState(change || !platform.configured);
  const [draft, setDraft] = useState({});
  const [error, setError] = useState('');
  const saved = savedBotSummary(platformKey, platform);

  const save = async () => {
    const body = credentialsPayload(platformKey, draft);
    if (!body) {
      setError('请先填写凭据');
      return;
    }
    setError('');
    const ok = await run(`${platformKey}-credentials`, async () => {
      const result = await api.setChannelCredentials(platformKey, body);
      setDraft({});
      return result;
    }, setError);
    if (ok) onNext();
  };

  if (!editing) {
    return (
      <>
        <p className="mb-3 text-[12px] text-fg-mute">使用已保存的机器人,或者换一个。</p>
        <SavedBot title={saved.title} desc={saved.desc}>
          <button type="button" className={linkButtonClass} onClick={() => setEditing(true)}>更换</button>
        </SavedBot>
        <WizardFooter>
          <button type="button" className={secondaryButtonClass} onClick={onCancel}>取消</button>
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} onClick={onNext}>下一步</button>
        </WizardFooter>
      </>
    );
  }
  return (
    <>
      <Instructions items={setup.credential.instructions} />
      <ExternalLinks links={setup.credential.links} />
      {setup.credential.note && <p className="mt-2 text-[11px] leading-5 text-fg-mute">{setup.credential.note}</p>}
      <div className="mt-1">
        <CredentialFields platformKey={platformKey} platform={platform} draft={draft} setDraft={setDraft} autoFocus />
      </div>
      <InlineError text={error} />
      {platform.configured && !change && (
        <button type="button" className={clsx(linkButtonClass, 'mt-2')} onClick={() => setEditing(false)}>
          使用已保存的机器人
        </button>
      )}
      <WizardFooter>
        <button type="button" className={secondaryButtonClass} onClick={onCancel}>取消</button>
        <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} disabled={!!busy} onClick={save}>
          {busy === `${platformKey}-credentials` ? '验证中…' : '验证并继续'}
        </button>
      </WizardFooter>
    </>
  );
}

// 第一步(扫码):QQ 与微信。QQ 另有手动填写;已保存的机器人默认显示摘要。
function ScanCredentialsStep({ platformKey, platform, bind, busy, run, change, onNext, onCancel }) {
  const setup = platformSetup(platformKey);
  const manualAvailable = setup.credential.fields.length > 0;
  const [mode, setMode] = useState(change || !platform.configured ? 'scan' : 'saved');
  const [draft, setDraft] = useState({});
  const [error, setError] = useState('');
  const view = bindView(bind, platformKey);
  const startedRef = useRef(false);
  const sawActiveRef = useRef(false);
  const saved = savedBotSummary(platformKey, platform);

  const startScan = useCallback(() => {
    startedRef.current = true;
    sawActiveRef.current = false;
    run(`${platformKey}-bind`, () => api.startChannelBind(platformKey));
  }, [platformKey, run]);

  // 用户已经点了「连接」:进入扫码方式就直接获取二维码,不用再点一次。
  useEffect(() => {
    if (mode !== 'scan' || startedRef.current || view.active || busy) return;
    startScan();
  }, [mode, view.active, busy, startScan]);

  // 只认这次发起的扫码:看到进行中之后再完成,才进入下一步(凭据已保存,通道已自动打开)。
  useEffect(() => {
    if (view.active) sawActiveRef.current = true;
    if (mode === 'scan' && bind.phase === 'completed' && sawActiveRef.current) onNext();
  }, [mode, view.active, bind.phase, onNext]);

  const leaveScan = (next) => {
    if (view.active) api.cancelChannelBind(platformKey).catch(() => {});
    if (next !== 'scan') startedRef.current = false;
    setMode(next);
  };

  const saveManual = async () => {
    const body = credentialsPayload(platformKey, draft);
    if (!body) {
      setError('请先填写凭据');
      return;
    }
    setError('');
    const ok = await run(`${platformKey}-credentials`, async () => {
      const result = await api.setChannelCredentials(platformKey, body);
      setDraft({});
      return result;
    }, setError);
    if (ok) onNext();
  };

  if (mode === 'saved') {
    return (
      <>
        <p className="mb-3 text-[12px] text-fg-mute">使用已保存的机器人,或者重新扫码换一个。</p>
        <SavedBot title={saved.title} desc={saved.desc}>
          <button type="button" className={linkButtonClass} onClick={() => setMode('scan')}>重新扫码</button>
          {manualAvailable && (
            <button type="button" className={linkButtonClass} onClick={() => setMode('manual')}>手动填写</button>
          )}
        </SavedBot>
        <WizardFooter>
          <button type="button" className={secondaryButtonClass} onClick={onCancel}>取消</button>
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} onClick={onNext}>下一步</button>
        </WizardFooter>
      </>
    );
  }
  if (mode === 'manual') {
    return (
      <>
        <p className="mb-1 text-[12px] text-fg-mute">{setup.credential.manualIntro}</p>
        <CredentialFields platformKey={platformKey} platform={platform} draft={draft} setDraft={setDraft} autoFocus />
        <InlineError text={error} />
        <button type="button" className={clsx(linkButtonClass, 'mt-2')} onClick={() => setMode('scan')}>返回扫码</button>
        <WizardFooter>
          <button type="button" className={secondaryButtonClass} onClick={onCancel}>取消</button>
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} disabled={!!busy}
            onClick={saveManual}>
            {busy === `${platformKey}-credentials` ? '验证中…' : '验证并继续'}
          </button>
        </WizardFooter>
      </>
    );
  }
  const retryable = ['failed', 'timed_out', 'cancelled'].includes(bind.phase) && !view.active;
  return (
    <>
      <div className="flex items-center gap-4">
        {view.showQr ? <QrCode value={bind.qrUrl} label="扫码连接二维码" /> : <QrPlaceholder />}
        <div className="min-w-0 flex-1 text-[12px] leading-5">
          <p className={clsx(TONE_TEXT[view.tone] || 'text-fg-2')}>{view.message || '正在获取二维码…'}</p>
          <p className="mt-2 text-fg-mute">{setup.credential.scanHint}</p>
          {retryable && (
            <button type="button" className={clsx(secondaryButtonClass, 'mt-3')} disabled={!!busy} onClick={startScan}>
              重新获取二维码
            </button>
          )}
        </div>
      </div>
      <div className="mt-3 flex items-center gap-2">
        {manualAvailable && (
          <button type="button" className={linkButtonClass} onClick={() => leaveScan('manual')}>手动填写</button>
        )}
        {platform.configured && !change && (
          <button type="button" className={linkButtonClass} onClick={() => leaveScan('saved')}>使用已保存的机器人</button>
        )}
      </div>
      <WizardFooter>
        <button type="button" className={secondaryButtonClass} onClick={onCancel}>取消</button>
      </WizardFooter>
    </>
  );
}

function ConnectStep({ platformKey, platform, busy, run, onBack, onNext }) {
  const view = connectStepView(platform);
  const setup = platformSetup(platformKey);
  const inviteUrl = platformKey === 'discord' ? discordInviteUrl(platform.credentialsPublic?.application_id) : '';
  // 连上后还要回平台后台做事(飞书保存长连接、Discord 邀请进服务器)时停在这一步,让用户看完再下一步。
  const checklist = setup.connectChecklist;
  const holdAfterConnect = checklist.length > 0 || Boolean(inviteUrl);
  const [error, setError] = useState('');
  const enabledRef = useRef(false);
  // 已经连着进来(从下一步退回)时不自动前进,否则「上一步」会被立刻弹回去。
  const autoAdvanceRef = useRef(view.phase !== 'connected' && !holdAfterConnect);
  const connect = useCallback(() => {
    setError('');
    run(`${platformKey}-toggle`, () => api.setChannelEnabled(platformKey, true), setError);
  }, [platformKey, run]);

  // 进入这一步就打开连接;扫码成功时 daemon 已经自动打开,不再重复。
  useEffect(() => {
    if (enabledRef.current || platform.enabled || busy) return;
    enabledRef.current = true;
    connect();
  }, [platform.enabled, busy, connect]);

  useEffect(() => {
    if (view.phase === 'connected' && autoAdvanceRef.current) onNext();
  }, [view.phase, onNext]);

  const working = view.phase === 'idle' || view.phase === 'connecting';
  return (
    <>
      <div className="flex items-start gap-3 rounded-md bg-surface-alt px-4 py-3">
        {working
          ? <span className="ace-spinner mt-0.5 shrink-0" />
          : <span className={clsx('mt-1.5 h-2 w-2 shrink-0 rounded-full', TONE_DOT[view.tone] || TONE_DOT.mute)} />}
        <p className={clsx('text-[12px] leading-5', TONE_TEXT[view.tone] || 'text-fg-2')}>{view.text}</p>
      </div>
      {view.phase === 'connected' && checklist.length > 0 && (
        <div className="mt-4">
          <div className="mb-1 text-[12px] font-medium">还差几步</div>
          <Instructions items={checklist} />
        </div>
      )}
      {inviteUrl && (
        <div className="mt-4 text-[12px] leading-5">
          <div className="mb-1 font-medium">把机器人拉进你的服务器</div>
          <a href={inviteUrl} target="_blank" rel="noreferrer" className="break-all text-accent hover:underline">{inviteUrl}</a>
        </div>
      )}
      <InlineError text={error} />
      <WizardFooter>
        <button type="button" className={secondaryButtonClass} onClick={onBack}>上一步</button>
        {view.phase === 'webhook' && (
          <button type="button" className={dangerButtonClass} disabled={!!busy}
            onClick={() => run('tg-webhook', () => api.removeTelegramWebhook(), setError)}>
            移除 webhook 并连接
          </button>
        )}
        {view.phase === 'failed' && (
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} disabled={!!busy}
            onClick={connect}>重试</button>
        )}
        {view.phase === 'connected' && (
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} onClick={onNext}>下一步</button>
        )}
      </WizardFooter>
    </>
  );
}

function OwnerStep({ platformKey, platform, busy, run, onDecide, onBack, onDone }) {
  const view = ownerStepView(platform, platformKey);
  const setup = platformSetup(platformKey);
  const pending = pendingRows(platform).filter((request) => request.kind === 'user');
  const [issued, setIssued] = useState(null);  // {link} 或 {code}
  const [error, setError] = useState('');
  const requestedRef = useRef(false);

  const issue = useCallback(() => {
    setError('');
    run(`${platformKey}-owner-link`, async () => {
      const result = await api.createChannelOwnerLink(platformKey);
      setIssued({ link: result?.link || '', code: result?.code || '' });
    }, setError);
  }, [platformKey, run]);

  useEffect(() => {
    if ((view.mode !== 'link' && view.mode !== 'code') || requestedRef.current || busy) return;
    requestedRef.current = true;
    issue();
  }, [view.mode, busy, issue]);

  const copy = async (value) => {
    const result = await copyTextToSystemClipboard(value);
    toast(result?.ok ? { kind: 'ok', text: '已复制' } : { kind: 'err', text: '复制失败,请手动选中复制' });
  };

  if (view.done) {
    return (
      <>
        <div className="flex flex-col items-center px-4 py-3 text-center">
          <span className="flex h-10 w-10 items-center justify-center rounded-full bg-accent-bg text-accent">
            <VsIcon name="check" size={18} />
          </span>
          <div className="mt-3 text-[14px] font-semibold">连接完成</div>
          <p className="mt-1 text-[12px] text-fg-2">{view.text}</p>
          <p className="mt-2 text-[12px] leading-5 text-fg-mute">{setup.doneHint}</p>
        </div>
        <WizardFooter>
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} onClick={onDone}>完成</button>
        </WizardFooter>
      </>
    );
  }
  const link = issued?.link || '';
  const code = issued?.code || '';
  return (
    <>
      {view.mode === 'link' && (
        <div className="flex items-center gap-4">
          {link ? <QrCode value={link} label="机主绑定二维码" /> : <QrPlaceholder />}
          <div className="min-w-0 flex-1 text-[12px] leading-5">
            <p>用手机相机扫码(Telegram 自带的扫码扫不了这个),在 Telegram 里点「开始」就成为机主。</p>
            <p className="mt-2 text-fg-mute">电脑上登录了 Telegram 也可以直接点链接:</p>
            {link && (
              <a href={link} target="_blank" rel="noreferrer" className="break-all text-accent hover:underline">{link}</a>
            )}
            <div className="mt-2 flex flex-wrap gap-2">
              <button type="button" className={secondaryButtonClass} disabled={!link} onClick={() => copy(link)}>复制链接</button>
              <button type="button" className={secondaryButtonClass} disabled={!!busy} onClick={issue}>重新生成</button>
            </div>
            <p className="mt-2 text-[11px] text-fg-mute">链接 10 分钟内有效,只能用一次。</p>
          </div>
        </div>
      )}
      {view.mode === 'code' && (
        <div className="rounded-md bg-surface-alt px-4 py-4 text-center">
          <p className="text-[12px] leading-5 text-fg-2">{view.text}</p>
          <div className="mt-3 font-mono text-[28px] tracking-[0.3em] text-fg" aria-label="绑定码">
            {code || <span className="ace-spinner" />}
          </div>
          <div className="mt-3 flex justify-center gap-2">
            <button type="button" className={secondaryButtonClass} disabled={!code} onClick={() => copy(code)}>复制</button>
            <button type="button" className={secondaryButtonClass} disabled={!!busy} onClick={issue}>重新生成</button>
          </div>
          <p className="mt-2 text-[11px] text-fg-mute">绑定码 10 分钟内有效,只能用一次;发送后这里会自动完成。</p>
        </div>
      )}
      {(view.mode === 'window' || view.mode === 'request') && (
        <p className="rounded-md bg-surface-alt px-4 py-3 text-[12px] leading-5 text-fg-2">{view.text}</p>
      )}
      <InlineError text={error} />
      {pending.length > 0 && (
        <div className="mt-4">
          <div className="text-[12px] font-medium">或者批准私聊请求,第一个被批准的人成为机主</div>
          <PendingList requests={pending} busy={busy} onDecide={onDecide} />
        </div>
      )}
      <WizardFooter>
        <button type="button" className={secondaryButtonClass} onClick={onBack}>上一步</button>
        <button type="button" className={secondaryButtonClass} onClick={onDone}>稍后再说</button>
      </WizardFooter>
    </>
  );
}

function ConnectWizard({ platformKey, platform, bind, busy, run, onDecide, startAt, change, onClose }) {
  const card = channelCards().find((item) => item.platform === platformKey);
  const setup = platformSetup(platformKey);
  const [step, setStep] = useState(startAt);
  const titleId = `ace-channels-wizard-${platformKey}`;
  const scanning = setup.credential.kind === 'scan' && bindView(bind, platformKey).active;
  const close = () => {
    if (scanning) api.cancelChannelBind(platformKey).catch(() => {});
    onClose();
  };
  const toConnect = useCallback(() => setStep(1), []);
  const toOwner = useCallback(() => setStep(2), []);

  return (
    <Modal onClose={close} width={540} labelledBy={titleId}>
      <div className="p-5">
        <div className="mb-4 flex items-center gap-3">
          <PlatformIcon name={card.icon} size={16} box="h-8 w-8" />
          <div id={titleId} className="flex-1 text-[15px] font-semibold">{`连接 ${card.title}`}</div>
          <button type="button" className={iconButtonClass} aria-label="关闭" onClick={close}>
            <VsIcon name="close" size={14} />
          </button>
        </div>
        <Stepper steps={setup.steps} current={step} />
        {step === 0 && setup.credential.kind === 'scan' && (
          <ScanCredentialsStep platformKey={platformKey} platform={platform} bind={bind} busy={busy} run={run}
            change={change} onNext={toConnect} onCancel={close} />
        )}
        {step === 0 && setup.credential.kind === 'form' && (
          <FormCredentialsStep platformKey={platformKey} platform={platform} busy={busy} run={run} change={change}
            onNext={toConnect} onCancel={close} />
        )}
        {step === 1 && (
          <ConnectStep platformKey={platformKey} platform={platform} busy={busy} run={run}
            onBack={() => setStep(0)} onNext={toOwner} />
        )}
        {step === 2 && (
          <OwnerStep platformKey={platformKey} platform={platform} busy={busy} run={run} onDecide={onDecide}
            onBack={() => setStep(1)} onDone={onClose} />
        )}
      </div>
    </Modal>
  );
}

function ManageDialog({ platformKey, platform, busy, onRevoke, onChangeBot, onClose }) {
  const card = channelCards().find((item) => item.platform === platformKey);
  const contacts = contactRows(platform);
  const titleId = `ace-channels-manage-${platformKey}`;
  const bot = savedBotSummary(platformKey, platform);
  return (
    <Modal onClose={onClose} width={480} labelledBy={titleId}>
      <div className="p-5">
        <div className="mb-4 flex items-center gap-3">
          <PlatformIcon name={card.icon} size={16} box="h-8 w-8" />
          <div id={titleId} className="flex-1 text-[15px] font-semibold">{`管理 ${card.title}`}</div>
          <button type="button" className={iconButtonClass} aria-label="关闭" onClick={onClose}>
            <VsIcon name="close" size={14} />
          </button>
        </div>
        <div className="mb-1 text-[12px] font-semibold">机器人</div>
        <SavedBot title={bot.title} desc={bot.desc}>
          <button type="button" className={secondaryButtonClass} disabled={!!busy} onClick={onChangeBot}>更换机器人</button>
        </SavedBot>
        <div className="mt-4 mb-1 text-[12px] font-semibold">机主与授权名单</div>
        <p className="mb-1 text-[11px] text-fg-mute">机主可以切换到任意会话;其他人只能在自己创建的会话之间切换。</p>
        {contacts.length === 0 ? (
          <p className="py-1.5 text-[12px] text-fg-mute">还没有授权任何人。</p>
        ) : contacts.map((contact) => (
          <div key={contact.principal} className="flex items-center justify-between gap-3 py-1.5">
            <div className="min-w-0">
              <div className="text-[12px] truncate">{contact.title}</div>
              <div className="text-[11px] text-fg-mute truncate">
                {[contact.kindLabel, contact.subtitle].filter(Boolean).join(' · ')}
              </div>
            </div>
            <button type="button" className={clsx(linkButtonClass, 'text-danger')} disabled={!!busy}
              onClick={() => onRevoke(contact)}>撤销</button>
          </div>
        ))}
        <WizardFooter>
          <button type="button" data-ace-dialog-primary="true" className={primaryButtonClass} onClick={onClose}>完成</button>
        </WizardFooter>
      </div>
    </Modal>
  );
}

export function ChannelsSettings() {
  const [state, setState] = useState(() => normalizeChannelsSnapshot(null));
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [busy, setBusy] = useState('');
  const busyRef = useRef('');
  const [wizard, setWizard] = useState(null);      // { platform, startAt, change }
  const [managing, setManaging] = useState('');   // 平台名
  const [revoking, setRevoking] = useState(null);  // { platform, contact }

  const load = useCallback(async () => {
    try {
      setState(normalizeChannelsSnapshot(await api.getChannels()));
      setError('');
    } catch (e) {
      setError(channelErrorText(e));
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => { load(); }, [load]);

  useEffect(() => {
    const handler = (event) => setState((prev) => applyChannelsEvent(prev, event.detail || {}));
    connection.addEventListener('message', handler);
    return () => connection.removeEventListener('message', handler);
  }, []);

  // 执行一个操作:同一时间只允许一个;返回快照的接口直接刷新页面,其余靠 WS 推送。
  // onError 给了就把错误交给调用方(向导里就地显示),否则弹提示。
  const run = useCallback(async (key, action, onError) => {
    if (busyRef.current) return false;
    busyRef.current = key;
    setBusy(key);
    try {
      const result = await action();
      if (result && Array.isArray(result.platforms)) {
        setState(normalizeChannelsSnapshot(result));
      } else if (result?.platform && result.phase) {
        // 扫码发起 / 取消
        setState((prev) => ({ ...prev, binds: { ...prev.binds, [result.platform]: normalizeBind(result) } }));
      }
      return true;
    } catch (e) {
      const message = channelErrorText(e);
      if (onError) onError(message);
      else toast({ kind: 'err', text: message });
      return false;
    } finally {
      busyRef.current = '';
      setBusy('');
    }
  }, []);

  const decide = useCallback((platformKey, request, approve) => run(`decide-${request.id}`, async () => {
    const result = await api.decideChannelRequest(platformKey, request.id, approve);
    toast({ kind: 'ok', text: approve ? `已批准 ${request.title}` : `已拒绝 ${request.title}` });
    return result;
  }), [run]);

  const connect = (platformKey) => {
    const platform = state.platforms[platformKey];
    if (connectNeedsWizard(platform)) {
      setWizard({ platform: platformKey, startAt: initialWizardStep(platform), change: false });
      return;
    }
    run(`${platformKey}-toggle`, () => api.setChannelEnabled(platformKey, true));
  };

  return (
    <>
      <h2 className="text-xl font-bold mb-2">消息通道</h2>
      <p className="text-[12px] text-fg-mute mb-5">
        在 QQ、微信、飞书、钉钉、Telegram、Discord、LINE 里远程使用 ACECode。凭据只保存在这台电脑上;关闭 Desktop 后是否继续在线,取决于 设置 &gt; 常规 里的“退出 ACECode 后继续运行后台进程”。
      </p>
      {error && (
        <div className="mb-3 px-3 py-2 rounded-md border border-danger bg-surface text-danger text-[12px] flex items-center justify-between gap-3">
          <span>{error}</span>
          <button type="button" className={linkButtonClass} onClick={() => { setLoading(true); load(); }}>重试</button>
        </div>
      )}
      {loading ? (
        <div className="px-3.5 py-8 text-[12px] text-fg-mute text-center">
          <span className="ace-spinner mr-2" /> 加载中
        </div>
      ) : (
        <div data-channel-cards="true" className="grid grid-cols-2 gap-3">
          {channelCards().map((card) => (
            <ChannelCard key={card.platform} card={card} platform={state.platforms[card.platform]} busy={busy}
              onConnect={() => connect(card.platform)}
              onDisconnect={() => run(`${card.platform}-toggle`, () => api.setChannelEnabled(card.platform, false))}
              onFix={() => setWizard({ platform: card.platform, startAt: 1, change: false })}
              onManage={() => setManaging(card.platform)}
              onDecide={(request, approve) => decide(card.platform, request, approve)} />
          ))}
        </div>
      )}
      {wizard && (
        <ConnectWizard platformKey={wizard.platform} platform={state.platforms[wizard.platform]}
          bind={state.binds[wizard.platform] || normalizeBind({ platform: wizard.platform })}
          busy={busy} run={run} startAt={wizard.startAt} change={wizard.change}
          onDecide={(request, approve) => decide(wizard.platform, request, approve)}
          onClose={() => setWizard(null)} />
      )}
      {managing && (
        <ManageDialog platformKey={managing} platform={state.platforms[managing]} busy={busy}
          onRevoke={(contact) => setRevoking({ platform: managing, contact })}
          onChangeBot={() => {
            setWizard({ platform: managing, startAt: 0, change: true });
            setManaging('');
          }}
          onClose={() => setManaging('')} />
      )}
      {revoking && (
        <Modal onClose={() => setRevoking(null)} width={420} labelledBy="ace-channels-revoke-title">
          <div className="p-4">
            <div id="ace-channels-revoke-title" className="text-[14px] font-semibold mb-2">
              {`撤销 ${revoking.contact.title} 的授权?`}
            </div>
            <div className="text-[12px] text-fg-mute mb-4">撤销后立即停止处理其消息,并中止其会话正在进行的回合。</div>
            <div className="flex justify-end gap-2">
              <button type="button" className={secondaryButtonClass} onClick={() => setRevoking(null)}>取消</button>
              <button type="button" data-ace-dialog-primary="true" className={dangerButtonClass} disabled={!!busy}
                onClick={async () => {
                  const ok = await run(`revoke-${revoking.contact.principal}`,
                    () => api.revokeChannelAccess(revoking.platform, revoking.contact.principal));
                  if (ok) setRevoking(null);
                }}>撤销</button>
            </div>
          </div>
        </Modal>
      )}
    </>
  );
}
