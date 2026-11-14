import { useEffect, useRef, useState } from 'react';
import { Modal } from './Modal.jsx';
import { Message } from './Message.jsx';
import { ShareDialogHeader } from './ShareSessionDialog.jsx';

export function RemoteSessionView({ api, peer, onClose }) {
  const [history, setHistory] = useState({ messages: [], before: '', hasOlder: false });
  const { messages, before, hasOlder } = history;
  const [loading, setLoading] = useState(true);
  const [connected, setConnected] = useState(false);
  const [allowQuestions, setAllowQuestions] = useState(peer.allow_questions);
  const [question, setQuestion] = useState('');
  const [answer, setAnswer] = useState('');
  const [discussion, setDiscussion] = useState([]);
  const [activeQuestion, setActiveQuestion] = useState('');
  const activeQuestionRef = useRef('');
  const [asking, setAsking] = useState(false);
  const [liveText, setLiveText] = useState('');
  const [error, setError] = useState('');
  const [quotaReached, setQuotaReached] = useState(false);
  const socket = useRef(null);
  const generation = useRef(0);
  useEffect(() => {
    generation.current += 1;
    setHistory({ messages: [], before: '', hasOlder: false });
    setDiscussion([]); setActiveQuestion(''); activeQuestionRef.current = '';
    setQuestion(''); setAnswer(''); setLiveText(''); setError(''); setQuotaReached(false);
    setConnected(false); setAllowQuestions(peer.allow_questions); setAsking(false);
    let active = true;
    let timer;
    let quotaTimer;
    const applyQuota = (data) => {
      if (data.remaining !== 0 && data.error !== 'daily_limit') return;
      setQuotaReached(true);
      const reset = Number(data.resets_at) * 1000;
      setError(Number.isFinite(reset) ? `提问额度已用完，将于 ${new Date(reset).toLocaleString()} 恢复` : '今日提问额度已用完');
      if (Number.isFinite(reset)) {
        clearTimeout(quotaTimer);
        quotaTimer = setTimeout(() => { setQuotaReached(false); setError(''); }, Math.max(0, reset - Date.now()));
      }
    };
    let request = 0;
    const reload = async () => {
      const generation = ++request;
      try {
        const page = await api.peerMessages(peer.id);
        if (!active || generation !== request) return;
        setHistory((previous) => {
          const newest = new Set(page.messages.map((message) => message.id));
          const first = previous.messages.findIndex((message) => newest.has(message.id));
          return { messages: [...(first > 0 ? previous.messages.slice(0, first) : []), ...page.messages],
            before: first > 0 ? previous.before : page.before, hasOlder: first > 0 ? previous.hasOlder : page.has_older };
        });
        setLiveText('');
      } catch { if (active) setError('无法读取远程会话，请确认连接或向对方索取新链接'); }
      finally { if (active) setLoading(false); }
    };
    void reload();
    const ws = api.peerSocket(peer.id); socket.current = ws;
    ws.onmessage = (event) => {
      if (!active) return;
      let data;
      try { data = JSON.parse(event.data); } catch { return; }
      if (data.type === 'hello') { setConnected(true); setAllowQuestions(!!data.allow_questions); setQuotaReached(false); applyQuota(data); }
      else if (data.type === 'changed') { clearTimeout(timer); timer = setTimeout(reload, 100); }
      else if (data.type === 'token') setLiveText((text) => text + (data.text || ''));
      else if (data.type === 'ask_delta') setAnswer((text) => (data.reset ? '' : text) + (data.text || ''));
      else if (data.type === 'ask_done') {
        setDiscussion((previous) => [...previous, { question: activeQuestionRef.current, answer: data.answer || '' }].slice(-10));
        setAnswer(''); setActiveQuestion(''); setAsking(false);
        if (data.error) setError('提问失败，请重试');
        applyQuota(data);
      }
      else if (data.type === 'error') {
        setAsking(false);
        setQuestion((draft) => draft || activeQuestionRef.current); setActiveQuestion('');
        if (data.error === 'daily_limit') applyQuota(data);
        else if (data.error === 'read_only') { setAllowQuestions(false); setError('此链接仅允许查看'); }
        else setError('提问失败，请重试');
      }
    };
    ws.onclose = () => { if (active) { setConnected(false); setAsking(false); setError('连接已断开，请确认分享仍然有效'); } };
    return () => { active = false; generation.current += 1; clearTimeout(timer); clearTimeout(quotaTimer); ws.close(); socket.current = null; };
  }, [api, peer]);
  const ask = (event) => {
    event.preventDefault();
    if (!question.trim() || asking || !allowQuestions || !connected || quotaReached) return;
    setError(''); setAnswer(''); setAsking(true);
    activeQuestionRef.current = question; setActiveQuestion(question); setQuestion('');
    socket.current?.send(JSON.stringify({ type: 'ask_start', question }));
  };
  return <Modal onClose={onClose} dismissOnBackdrop={false} width={1000} labelledBy="remote-session-title">
    <div className="p-5 flex flex-col h-[82vh] min-h-0" data-remote-session-view="true">
      <ShareDialogHeader title={`${peer.owner} · ${peer.title}`} onClose={onClose} id="remote-session-title" />
      <div className="flex-1 overflow-y-auto min-h-0 space-y-4 px-1">
        {hasOlder && before && <button type="button" className="text-[13px] text-accent hover:underline" disabled={loading} onClick={async () => {
          setLoading(true);
          const requestGeneration = generation.current;
          try { const page = await api.peerMessages(peer.id, before); if (requestGeneration === generation.current) setHistory((old) => old.before === before ? { messages: [...page.messages, ...old.messages], before: page.before, hasOlder: page.has_older } : old); }
          catch { if (requestGeneration === generation.current) setError('无法读取远程会话，请确认连接或向对方索取新链接'); } finally { if (requestGeneration === generation.current) setLoading(false); }
        }}>加载更早的消息</button>}
        {loading && !messages.length && <div role="status" className="text-[13px] text-fg-mute">正在加载</div>}
        {messages.map((message, index) => message.role === 'tool' ? (
          <details key={message.id || index} className="text-[13px] text-fg-mute"><summary>{message.metadata?.tool_name || '工具'}{typeof message.metadata?.tool_summary === 'string' ? ` · ${message.metadata.tool_summary}` : ''}</summary>{message.content && <pre className="whitespace-pre-wrap break-words font-mono text-[12px] mt-2">{message.content}</pre>}</details>
        ) : <div key={message.id || index} className="space-y-2">
          <Message role={message.role} content={message.content || ''} externalContent showFooter={false} />
          {(message.content_parts || []).filter((part) => part.type === 'image' && typeof part.attachment?.id === 'string').map((part) => (
            <img key={part.attachment.id} src={api.peerAttachmentUrl(peer.id, part.attachment.id)} alt={part.attachment.name || 'Image'} loading="lazy" className="max-w-full max-h-96 rounded-md object-contain" />
          ))}
        </div>)}
        {liveText && <Message role="assistant" content={liveText} externalContent streaming showFooter={false} />}
        {discussion.map((turn, index) => <div key={`question-${index}`} className="space-y-3 pt-4 border-t border-border">
          <Message role="user" content={turn.question} showFooter={false} />
          <Message role="assistant" content={turn.answer} externalContent showFooter={false} />
        </div>)}
        {activeQuestion && <div className="space-y-3 pt-4 border-t border-border">
          <Message role="user" content={activeQuestion} showFooter={false} />
          {answer && <Message role="assistant" content={answer} externalContent streaming={asking} showFooter={false} />}
        </div>}
      </div>
      {error && <div role="alert" className="text-[13px] text-danger mt-3">{error}</div>}
      {allowQuestions && <form className="flex gap-2 mt-4 pt-4 border-t border-border" onSubmit={ask} data-guest-question="true">
        <textarea aria-label="向此会话提问" placeholder="向此会话提问" value={question} onChange={(event) => setQuestion(event.target.value)} rows={2} maxLength={16000}
          className="flex-1 min-w-0 resize-none rounded-md border border-border bg-surface-alt px-3 py-2 text-[13px] outline-none focus:border-accent"
          disabled={!connected || asking || quotaReached} onKeyDown={(event) => { if (event.key === 'Enter' && !event.shiftKey && !event.nativeEvent.isComposing) ask(event); }} />
        {asking ? <button type="button" className="px-3 text-[13px] text-danger" onClick={() => socket.current?.send(JSON.stringify({ type: 'ask_stop' }))}>停止</button> :
          <button type="submit" className="px-3 rounded-md bg-accent text-white text-[13px] disabled:opacity-50" disabled={!connected || !question.trim() || quotaReached}>提问</button>}
      </form>}
    </div>
  </Modal>;
}
