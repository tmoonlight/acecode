import { useEffect, useRef, useState, useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { lookupErrorMessage } from '../lib/errors.js';
import { imageGenerationCanTest, imageGenerationPayload, imageGenerationSettingsStore } from '../lib/imageGenerationSettings.js';
import { Toggle } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { toast } from './Toast.jsx';

const fieldClass = 'w-full h-8 px-2 text-[12px] border border-border bg-surface-alt text-fg outline-none focus:border-accent disabled:opacity-50';
const buttonClass = 'px-3 py-1.5 text-[12px] border border-border bg-surface-alt hover:bg-surface-hi disabled:opacity-50';

export function ImageGenerationSettings() {
  const store = imageGenerationSettingsStore(api);
  const { snapshot, draft, loading, saving, error: saveError } = useSyncExternalStore(store.subscribe, store.getSnapshot);
  const [apiKeyVisible, setApiKeyVisible] = useState(false);
  const [expanded, setExpanded] = useState(false);
  const [testing, setTesting] = useState(false);
  const [testError, setTestError] = useState(null);
  const [preview, setPreview] = useState(null);
  const mounted = useRef(false);
  const pending = useRef(false);
  const error = testError || saveError;
  const errorMessage = error ? lookupErrorMessage(error.code,
    error.action === 'load' ? '加载图像生成配置失败' :
      error.action === 'save' ? '保存图像生成配置失败' : '生成测试图片失败，请检查连接和模型名称') : '';

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
        if (!ok) toast({ kind: 'err', text: '保存图像生成配置失败' });
      });
    };
  }, [store]);

  const update = (key, value, immediate = false) => {
    store.update(key, value);
    if (key === 'source') setApiKeyVisible(false);
    setTestError(null);
    setPreview(null);
    if (immediate) void store.flush();
  };
  const saveOnBlur = () => { void store.flush(); };
  const test = async () => {
    if (pending.current) return;
    pending.current = true;
    setTesting(true);
    setTestError(null);
    setPreview(null);
    try {
      if (!await store.flush() || !mounted.current) return;
      const result = await api.testImageGeneration(imageGenerationPayload(store.getSnapshot().draft));
      if (mounted.current) setPreview(result);
    } catch (e) {
      if (mounted.current) setTestError({ code: e.code, action: 'test' });
    } finally {
      pending.current = false;
      if (mounted.current) setTesting(false);
    }
  };

  return (
    <div className="bg-surface border border-border mb-2" aria-busy={loading || saving}>
      <div className="flex items-center gap-3 px-3.5 py-3">
        <div className="w-10 h-10 bg-surface-alt border border-border flex items-center justify-center shrink-0 text-fg">
          <VsIcon name="Image" size={20} />
        </div>
        <div className="flex-1 min-w-0">
          <div className="text-[13px] font-medium">图像生成</div>
          <div className="text-[11px] text-fg-mute mt-0.5">根据描述生成图片，或编辑已有图片</div>
        </div>
        <Toggle on={!!draft?.enabled}
          disabled={!snapshot || loading || testing} ariaLabel="启用图像生成"
          onChange={(enabled) => update('enabled', enabled, true)} />
        <button type="button" className="px-1.5 py-0.5 text-[11px] hover:underline disabled:opacity-50"
          disabled={!snapshot || loading} aria-expanded={expanded} aria-controls="image-generation-config"
          onClick={() => { if (expanded) void store.flush(); setExpanded((value) => !value); setApiKeyVisible(false); }}>{expanded ? '收起' : '配置'}</button>
      </div>

      {error && <div role="alert" className="px-3.5 pb-3 text-[12px] text-danger">
        {errorMessage}
        <button type="button" onClick={() => error.action === 'load' ? store.load() : error.action === 'save' ? store.flush() : test()}
          disabled={loading || saving || testing} className="ml-2 hover:underline">重试</button>
      </div>}

      {expanded && draft && <form id="image-generation-config" className="border-t border-border px-3.5 py-3"
        onSubmit={(event) => { event.preventDefault(); saveOnBlur(); }}>
        <fieldset disabled={loading || testing} className="space-y-3 min-w-0">
          <label className="block text-[12px] text-fg-mute">
            <span className="block mb-1">连接来源</span>
            <select aria-label="连接来源" className={fieldClass} value={draft.source} onChange={(e) => update('source', e.target.value, true)}>
              <option value="saved_model">复用已保存的模型连接</option>
              <option value="inline">单独配置</option>
            </select>
          </label>
          {draft.source === 'saved_model' ? <div>
            <label className="block text-[12px] text-fg-mute">
              <span className="block mb-1">模型连接</span>
              <select aria-label="模型连接" className={fieldClass} value={draft.saved_model_name} required
                onChange={(e) => update('saved_model_name', e.target.value, true)}>
                <option value="">选择模型连接</option>
                {draft.saved_model_name && !snapshot.connections.some((item) => item.name === draft.saved_model_name) &&
                  <option value={draft.saved_model_name} disabled>{draft.saved_model_name} · 连接不可用</option>}
                {snapshot.connections.map((item) => <option key={item.name} value={item.name}>{item.name}</option>)}
              </select>
            </label>
            <p className="text-[11px] text-fg-mute mt-1">复用连接的 API 地址和 Key，图像模型在下方单独指定。</p>
            {snapshot.connections.length === 0 && <p className="text-[11px] text-fg-mute mt-1">暂无可复用的 OpenAI 兼容连接，请选择单独配置。</p>}
          </div> : <>
            <label className="block text-[12px] text-fg-mute">
              <span className="block mb-1">API 地址</span>
              <input className={fieldClass} type="url" value={draft.base_url} placeholder="https://api.example.com/v1"
                onChange={(e) => update('base_url', e.target.value)} onBlur={saveOnBlur} autoComplete="off" spellCheck={false} />
            </label>
            <div>
              <label htmlFor="image-generation-api-key" className="block mb-1 text-[12px] text-fg-mute">API Key</label>
              <div className="relative">
                <input id="image-generation-api-key" className={`${fieldClass} pr-10`}
                  type={apiKeyVisible ? 'text' : 'password'} value={draft.apiKey} autoComplete="off" spellCheck={false}
                  placeholder="输入 API Key" onChange={(e) => update('apiKey', e.target.value)} onBlur={saveOnBlur} />
                <button type="button" onClick={() => setApiKeyVisible((value) => !value)}
                  aria-label={apiKeyVisible ? '隐藏 API Key' : '显示 API Key'} aria-pressed={apiKeyVisible}
                  aria-controls="image-generation-api-key" title={apiKeyVisible ? '隐藏 API Key' : '显示 API Key'}
                  className={`absolute inset-y-0 right-0 flex w-9 items-center justify-center transition hover:bg-surface-hi hover:text-fg focus:outline-none focus:ring-1 focus:ring-inset focus:ring-accent ${apiKeyVisible ? 'text-accent' : 'text-fg-mute'}`}>
                  <VsIcon name="eye" size={14} />
                </button>
              </div>
            </div>
          </>}
          <label className="block text-[12px] text-fg-mute">
            <span className="block mb-1">默认画质</span>
            <select aria-label="默认画质" className={fieldClass} value={draft.default_quality} onChange={(e) => update('default_quality', e.target.value, true)}>
              <option value="standard">标准</option><option value="high">高</option><option value="ultra">超高</option>
            </select>
          </label>
          <details>
            <summary className="text-[12px] text-fg-2 cursor-pointer">高级设置</summary>
            <div className="space-y-3 mt-3">
              {[
                ['standard', '标准画质模型'], ['high', '高画质模型'], ['ultra', '超高画质模型'],
              ].map(([key, label]) => <label key={key} className="block text-[12px] text-fg-mute">
                <span className="block mb-1">{label}</span>
                <input className={fieldClass} value={draft.models[key]} required spellCheck={false}
                  onChange={(e) => update(`models.${key}`, e.target.value)} onBlur={saveOnBlur} />
              </label>)}
              <label className="flex items-center justify-between text-[12px] text-fg-mute">
                <span>请求超时（秒）</span>
                <input className={`${fieldClass} max-w-24 text-center`} type="number" min={30} max={600} step={1} required
                  value={draft.timeout_ms === '' ? '' : draft.timeout_ms / 1000}
                  onChange={(e) => update('timeout_ms', e.target.value === '' ? '' : Number(e.target.value) * 1000)} onBlur={saveOnBlur} />
              </label>
            </div>
          </details>
          <div className="border-t border-border pt-3">
            <p className="text-[11px] text-fg-mute mb-2">测试会使用标准画质生成一张图片并消耗额度。</p>
            <div className="flex flex-wrap items-center gap-2">
              <button type="button" className={buttonClass} disabled={!imageGenerationCanTest(draft, snapshot)} onClick={test}>
                {testing ? '正在生成测试图片…' : '生成测试图片'}
              </button>
            </div>
          </div>
        </fieldset>
        {testing && <div role="status" className="mt-3 text-[12px] text-fg-mute">正在生成，请稍候…</div>}
        {preview && <div className="mt-3 border-t border-border pt-3">
          <div role="status" className="text-[12px] text-ok mb-2">测试图片已生成</div>
          <img src={preview.image_data_url} alt="图像生成测试结果" className="max-w-full max-h-64 object-contain border border-border"
            onError={() => { setPreview(null); setTestError({ code: 'IMAGE_TEST_FAILED', action: 'test' }); }} />
        </div>}
      </form>}
    </div>
  );
}
