import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { speechDraftValid, speechErrorMessage, speechPayload, speechSettingsStore } from '../lib/speechRecognition.js';
import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const fieldClass = 'w-full h-8 px-2 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent disabled:opacity-50';
const buttonClass = 'px-3 py-1.5 text-[12px] rounded-md border border-border bg-surface-alt hover:bg-surface-hi disabled:opacity-50';

export function SpeechRecognitionSettings() {
  const store = speechSettingsStore(api);
  const { draft, loading, saving, error } = useSyncExternalStore(store.subscribe, store.getSnapshot);
  const [expanded, setExpanded] = useState(false);
  const [testing, setTesting] = useState(false);
  const [testError, setTestError] = useState(null);
  const mounted = useRef(false);
  const pendingTest = useRef(false);
  useEffect(() => {
    mounted.current = true;
    void store.load();
    const flush = () => { void store.flush(); };
    window.addEventListener('blur', flush);
    window.addEventListener('pagehide', flush);
    return () => {
      mounted.current = false;
      window.removeEventListener('blur', flush);
      window.removeEventListener('pagehide', flush);
      void store.flush().then((ok) => {
        if (!ok) toast({ kind: 'err', text: speechErrorMessage(store.getSnapshot().error) });
      });
    };
  }, [store]);
  const update = (field, value, immediate = false) => {
    setTestError(null); store.update(field, value);
    if (immediate) void store.flush();
  };
  const saveOnBlur = () => { void store.flush(); };
  const test = async () => {
    if (pendingTest.current) return;
    pendingTest.current = true;
    setTesting(true); setTestError(null);
    try {
      if (!await store.flush() || !mounted.current) return;
      const current = store.getSnapshot().draft;
      if (!speechDraftValid(current, true)) { setTestError({ code: 'SPEECH_INVALID_CONFIG' }); return; }
      await api.testSpeechRecognition(speechPayload(current));
      if (mounted.current) toast({ kind: 'ok', text: '语音识别连接正常' });
    } catch (failure) { if (mounted.current) setTestError(failure); }
    finally { pendingTest.current = false; if (mounted.current) setTesting(false); }
  };
  return <section data-speech-settings aria-busy={loading || saving} className="rounded-md border border-border bg-surface mb-2">
    <div className="flex items-center gap-3 px-3.5 py-3 border border-border">
      <div data-settings-surface="true" className="w-10 h-10 rounded-md bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
        <VsIcon name="microphone" size={20} />
      </div>
      <div className="flex-1 min-w-0"><div className="text-[13px] font-normal text-fg">语音识别</div></div>
      <button type="button" aria-expanded={expanded} aria-controls="speech-recognition-config"
        className="px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50"
        onClick={() => { if (expanded) saveOnBlur(); setExpanded((value) => !value); }}>{expanded ? '收起' : '配置'}</button>
      <Toggle on={!!draft?.enabled} disabled={!draft || loading || testing}
        ariaLabel="启用语音识别" onChange={(enabled) => update('enabled', enabled, true)} />
    </div>
    {(error || testError) && <div role="alert" className="px-3.5 pb-3 text-[12px] text-danger">
      {speechErrorMessage(testError || error)}
      {error && <button type="button" disabled={loading || saving || testing}
        onClick={() => { void (error.action === 'load' ? store.load() : store.flush()); }} className="ml-2 hover:underline">重试</button>}
    </div>}
    {expanded && draft && <form id="speech-recognition-config" className="border-t border-border px-3.5 py-3"
      onSubmit={(event) => { event.preventDefault(); saveOnBlur(); }}
      onKeyDown={(event) => {
        if (event.key !== 'Enter' || event.target.tagName !== 'INPUT' || event.nativeEvent.isComposing || event.keyCode === 229) return;
        event.preventDefault(); saveOnBlur();
      }}>
      <fieldset disabled={loading || testing} className="space-y-3 min-w-0">
        <label className="block text-[12px] text-fg-mute">
          <span className="block mb-1">接口协议</span>
          <select aria-label="语音接口协议" className={fieldClass} value={draft.protocol} onChange={(e) => update('protocol', e.target.value, true)}>
            <option value="sensevoice">SenseVoice</option>
            <option value="openai">OpenAI 兼容</option>
            <option value="whisper_cpp">whisper.cpp</option>
          </select>
        </label>
        <label className="block text-[12px] text-fg-mute">
          <span className="block mb-1">服务地址</span>
          <input aria-label="语音服务地址" type="url" className={fieldClass} value={draft.base_url}
            placeholder="http://192.168.1.50:8765" autoComplete="off" spellCheck={false}
            onChange={(e) => update('base_url', e.target.value)} onBlur={saveOnBlur} />
        </label>
        {draft.protocol === 'openai' && <label className="block text-[12px] text-fg-mute">
          <span className="block mb-1">模型</span>
          <input aria-label="语音识别模型" className={fieldClass} value={draft.model} autoComplete="off" spellCheck={false}
            onChange={(e) => update('model', e.target.value)} onBlur={saveOnBlur} />
        </label>}
        <label className="block text-[12px] text-fg-mute">
          <span className="block mb-1">API Key（可选）</span>
          <input aria-label="语音 API Key" type="password" className={fieldClass} value={draft.api_key}
            autoComplete="off" spellCheck={false} onChange={(e) => update('api_key', e.target.value)} onBlur={saveOnBlur} />
        </label>
        <details>
          <summary className="text-[12px] text-fg-2 cursor-pointer">高级设置</summary>
          <div className="space-y-3 mt-3">
            <label className="flex items-center justify-between gap-3 text-[12px] text-fg-mute">
              <span>最长录音（秒）</span>
              <input aria-label="最长录音秒数" className={fieldClass + ' max-w-24 text-center'} type="number" min={1} max={300}
                value={draft.max_recording_seconds} onChange={(e) => update('max_recording_seconds', e.target.value)} onBlur={saveOnBlur} />
            </label>
            <label className="flex items-center justify-between gap-3 text-[12px] text-fg-mute">
              <span>请求超时（秒）</span>
              <input aria-label="语音请求超时秒数" className={fieldClass + ' max-w-24 text-center'} type="number" min={5} max={600}
                value={draft.timeout_ms === '' ? '' : draft.timeout_ms / 1000}
                onChange={(e) => update('timeout_ms', e.target.value === '' ? '' : Number(e.target.value) * 1000)} onBlur={saveOnBlur} />
            </label>
            <div className="flex items-center justify-between text-[12px] text-fg-mute">
              <span>使用 ACECode 代理</span>
              <Toggle on={draft.use_proxy} ariaLabel="语音识别使用代理" onChange={(value) => update('use_proxy', value, true)} />
            </div>
          </div>
        </details>
        <div className="flex items-center justify-end gap-2">
          <button type="button" className={buttonClass} disabled={!speechDraftValid(draft, true)} onClick={test}>{testing ? '测试中…' : '测试连接'}</button>
        </div>
      </fieldset>
    </form>}
  </section>;
}
