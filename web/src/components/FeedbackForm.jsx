import { useCallback, useEffect, useId, useMemo, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { clsx, relativeTime } from '../lib/format.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { sessionDisplayTitle } from '../lib/sessionTitle.js';
import {
  DESKTOP_FEEDBACK_MAX_CHARACTERS,
  NO_FEEDBACK_SESSION_KEY,
  buildDesktopFeedbackPayload,
  desktopFeedbackTextLength,
  feedbackSessionKey,
  normalizeDesktopFeedbackSessions,
  selectedFeedbackSessionFromKey,
} from '../lib/desktopFeedback.js';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

function sessionOptionLabel(item) {
  const title = sessionDisplayTitle(item, item?.title || item?.summary || item?.id || '');
  const when = relativeTime(item?.updated_at || item?.created_at);
  const workspace = item?.workspaceName || item?.cwd || item?.workspace_hash || '';
  return [title, when, workspace].filter(Boolean).join(' · ');
}

// An onClose callback presents the shared form as a standalone dialog.
export function FeedbackForm({ onClose }) {
  const formId = useId();
  const inputRef = useRef(null);
  const sessionLoadRef = useRef(0);
  const submittingRef = useRef(false);
  const [feedbackText, setFeedbackText] = useState('');
  const [sessions, setSessions] = useState([]);
  const [selectedKey, setSelectedKey] = useState(NO_FEEDBACK_SESSION_KEY);
  const [loadingSessions, setLoadingSessions] = useState(true);
  const [sessionsError, setSessionsError] = useState('');
  const [submitting, setSubmitting] = useState(false);
  const [submitError, setSubmitError] = useState('');
  const count = desktopFeedbackTextLength(feedbackText);
  const tooLong = count > DESKTOP_FEEDBACK_MAX_CHARACTERS;
  const selectedSession = useMemo(
    () => selectedFeedbackSessionFromKey(sessions, selectedKey),
    [sessions, selectedKey],
  );

  const loadSessions = useCallback(async () => {
    const request = ++sessionLoadRef.current;
    setLoadingSessions(true);
    setSessionsError('');
    try {
      const result = normalizeDesktopFeedbackSessions(await api.listDesktopFeedbackSessions(20));
      if (request !== sessionLoadRef.current) return;
      setSessions(result);
      setSelectedKey((key) => selectedFeedbackSessionFromKey(result, key) ? key : NO_FEEDBACK_SESSION_KEY);
    } catch (error) {
      if (request === sessionLoadRef.current) setSessionsError(error?.message || String(error));
    } finally {
      if (request === sessionLoadRef.current) setLoadingSessions(false);
    }
  }, []);

  useEffect(() => {
    loadSessions();
    inputRef.current?.focus();
    return () => { sessionLoadRef.current += 1; };
  }, [loadSessions]);

  const submit = async (event) => {
    event.preventDefault();
    if (submittingRef.current || tooLong) return;
    submittingRef.current = true;
    setSubmitting(true);
    setSubmitError('');
    try {
      await api.submitDesktopFeedback(buildDesktopFeedbackPayload({ feedbackText, selectedSession }));
      setFeedbackText('');
      setSelectedKey(NO_FEEDBACK_SESSION_KEY);
      toast({ kind: 'ok', text: '问题反馈已上传' });
      onClose?.();
    } catch (error) {
      setSubmitError(lookupErrorMessage(error?.code, error?.body?.message || error?.message || String(error)));
    } finally {
      submittingRef.current = false;
      setSubmitting(false);
    }
  };

  const content = (
    <form onSubmit={submit} aria-busy={submitting} data-feedback-form="true"
      className={clsx('flex flex-col gap-4', onClose ? 'p-5' : 'w-full min-w-0')}>
      <div className="flex items-center justify-between gap-3">
        <h2 id={`${formId}-title`} className="text-[18px] font-semibold">{onClose ? '意见反馈' : '问题反馈'}</h2>
        {onClose && (
          <button type="button" onClick={onClose} disabled={submitting} title="关闭" aria-label="关闭"
            className="h-7 w-7 shrink-0 flex items-center justify-center rounded-md text-fg-2 hover:bg-surface-hi hover:text-fg disabled:opacity-50">
            <VsIcon name="close" size={16} />
          </button>
        )}
      </div>
      <div>
        <label htmlFor={`${formId}-text`} className="sr-only">反馈内容</label>
        <div data-settings-surface="true" className={clsx('overflow-hidden rounded-lg border bg-bg focus-within:border-accent', tooLong ? 'border-danger' : 'border-border')}>
          <textarea ref={inputRef} id={`${formId}-text`} value={feedbackText}
            onChange={(event) => setFeedbackText(event.target.value)} disabled={submitting}
            placeholder="描述你遇到的问题" aria-describedby={`${formId}-count${tooLong ? ` ${formId}-limit` : ''}`}
            aria-invalid={tooLong || undefined}
            className="block w-full h-[min(220px,35vh)] min-h-28 resize-none bg-transparent px-3 pt-3 pb-2 text-[13px] leading-relaxed text-fg outline-none disabled:opacity-60" />
          <div id={`${formId}-count`} className={clsx('px-3 pb-2 text-right text-[11px] tabular-nums', tooLong ? 'text-danger' : 'text-fg-mute')}>
            {count}/{DESKTOP_FEEDBACK_MAX_CHARACTERS}
          </div>
        </div>
        {tooLong && <p id={`${formId}-limit`} className="mt-1.5 text-[12px] text-danger">反馈内容不能超过 10000 字</p>}
      </div>
      <div>
        <div className="flex items-center justify-between gap-3 mb-1.5">
          <label htmlFor={`${formId}-session`} className="text-[12px] text-fg-2">最近会话记录</label>
          <button type="button" onClick={loadSessions} disabled={loadingSessions || submitting} title="刷新" aria-label="刷新"
            className="w-6 h-6 flex items-center justify-center rounded-md text-fg-mute hover:bg-surface-hi hover:text-fg disabled:opacity-50">
            <VsIcon name="refresh" size={13} className={loadingSessions ? 'animate-spin' : ''} />
          </button>
        </div>
        <select id={`${formId}-session`} value={selectedKey} onChange={(event) => setSelectedKey(event.target.value)}
          disabled={loadingSessions || submitting}
          className="w-full h-8 rounded-md border border-border bg-bg px-2 text-[13px] text-fg outline-none focus:border-accent disabled:opacity-60">
          <option value={NO_FEEDBACK_SESSION_KEY}>不附带会话</option>
          {sessions.map((item) => <option key={feedbackSessionKey(item)} value={feedbackSessionKey(item)}>{sessionOptionLabel(item)}</option>)}
        </select>
        <p className="mt-1.5 text-[11px] text-fg-mute">默认附带近期日志；选择会话后，同时附带该会话的记录和发给模型的请求原文。</p>
        {sessionsError && <p role="alert" className="mt-1.5 break-words text-[12px] text-danger">加载会话失败:{sessionsError}</p>}
      </div>
      {submitError && <p role="alert" className="break-words text-[12px] text-danger">上传失败:{submitError}</p>}
      <div className="flex justify-end">
        <button type="submit" data-ace-dialog-primary="true" disabled={submitting || tooLong}
          className={clsx('h-8 px-3 flex items-center gap-1.5 rounded-md bg-accent text-white text-[13px] hover:opacity-90 disabled:opacity-50 disabled:cursor-not-allowed',
            onClose ? 'font-medium' : 'font-normal')}>
          {submitting ? <span className="ace-spinner" /> : <VsIcon name="send" size={13} />}
          提交反馈
        </button>
      </div>
    </form>
  );

  return onClose ? (
    <Modal width={560} onClose={() => { if (!submittingRef.current) onClose(); }}
      dismissOnBackdrop={!submitting} dismissOnEscape={!submitting} labelledBy={`${formId}-title`}>
      {content}
    </Modal>
  ) : content;
}
