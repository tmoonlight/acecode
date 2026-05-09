// web/src/components/ModelManager.jsx
// 左侧 saved_models 列表(默认带星);右侧编辑表单。增 / 改 / 删 / 设默认。
//
// api_key 编辑策略(配合 PUT /api/models/:name 的 patch 语义):
//   - 编辑模式打开时 api_key 字段预填 mask "••••••••"(纯视觉占位,不是真值)
//   - 用户聚焦后清空字段,等待输入;blur 时若没改回到 mask
//   - 提交时若 apiKeyTouched=false → payload 删 api_key,后端从 existing 注入
//   - apiKeyTouched=true → 发用户输入的新值(可能为空,后端会按校验拒绝)
import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';
import { lookupErrorMessage } from '../lib/errors.js';
import { validateModelDraft } from '../lib/modelManager.js';

const EMPTY_DRAFT = {
  name: '', provider: 'openai', model: '',
  base_url: '', api_key: '',
};
const API_KEY_MASK = '••••••••';

export function ModelManager({ apiClient = api }) {
  const [models, setModels] = useState([]);
  const [defaultName, setDefaultName] = useState('');
  const [editingName, setEditingName] = useState(null);
  const [draft, setDraft] = useState(EMPTY_DRAFT);
  const [apiKeyTouched, setApiKeyTouched] = useState(false);
  const [busy, setBusy] = useState(false);

  const refresh = async () => {
    try {
      const list = await apiClient.listModels();
      setModels(Array.isArray(list) ? list : []);
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    }
    try {
      const d = await apiClient.getDefaultModel();
      setDefaultName((d && d.name) || '');
    } catch {
      // 默认拿不到不致命,星标暂不显示
    }
  };

  useEffect(() => { refresh(); /* eslint-disable-next-line react-hooks/exhaustive-deps */ }, []);

  const onSelect = (m) => {
    if (m.is_legacy) {
      // (legacy) 行不可编辑 — 退回新增态。
      setEditingName(null);
      setDraft(EMPTY_DRAFT);
      setApiKeyTouched(true);
      return;
    }
    setEditingName(m.name);
    setApiKeyTouched(false);
    setDraft({
      name: m.name,
      provider: m.provider,
      model: m.model,
      base_url: m.base_url || '',
      api_key: API_KEY_MASK, // 视觉 mask;真值不在 GET 响应里返,后端走 patch
    });
  };

  const startNew = () => {
    setEditingName(null);
    setDraft(EMPTY_DRAFT);
    setApiKeyTouched(true); // 新增模式 api_key 必填,直接进 touched 状态
  };

  const onApiKeyFocus = () => {
    if (editingName && !apiKeyTouched) {
      // 进入编辑:清空 mask 让用户能直接输入新 key,但还没真的算 touched —
      // 用户 blur 而没输入就恢复 mask。
      setDraft((d) => ({ ...d, api_key: '' }));
    }
  };
  const onApiKeyChange = (e) => {
    setDraft((d) => ({ ...d, api_key: e.target.value }));
    setApiKeyTouched(true);
  };
  const onApiKeyBlur = () => {
    if (editingName && !apiKeyTouched) {
      setDraft((d) => ({ ...d, api_key: API_KEY_MASK }));
    }
  };

  const submit = async () => {
    // 编辑模式 + 没改 api_key:client-side 校验跳过它(后端 PUT 会从 existing 注入)。
    const skipApiKey = !!editingName && !apiKeyTouched;
    const draftForValidate = skipApiKey
      ? { ...draft, api_key: '__patch__' }  // 占位让 validateModelDraft 通过 openai 必填检查
      : draft;
    const v = validateModelDraft(draftForValidate);
    if (!v.ok) { toast({ kind: 'err', text: lookupErrorMessage(v.code) }); return; }
    setBusy(true);
    try {
      if (editingName) {
        const payload = { ...draft };
        if (skipApiKey) delete payload.api_key; // 后端 patch:缺字段保留旧值
        await apiClient.updateModel(editingName, payload);
      } else {
        await apiClient.addModel(draft);
      }
      toast({ kind: 'ok', text: editingName ? '已更新' : '已新增' });
      setEditingName(null);
      setDraft(EMPTY_DRAFT);
      setApiKeyTouched(true);
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  const removeOne = async (name) => {
    setBusy(true);
    try {
      await apiClient.removeModel(name);
      toast({ kind: 'ok', text: '已删除 ' + name });
      if (editingName === name) { setEditingName(null); setDraft(EMPTY_DRAFT); setApiKeyTouched(true); }
      await refresh();
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  const setDefault = async (name) => {
    setBusy(true);
    try {
      await apiClient.setDefaultModel(name);
      setDefaultName(name);
      toast({ kind: 'ok', text: '默认: ' + name });
    } catch (err) {
      toast({ kind: 'err', text: lookupErrorMessage(err && err.code, err && err.message) });
    } finally { setBusy(false); }
  };

  return (
    <div className="grid grid-cols-[260px_1fr] gap-4">
      <div className="border border-border rounded-md overflow-hidden">
        {models.map((m) => (
          <div
            key={m.name}
            className={
              'flex items-center justify-between px-2 py-1.5 cursor-pointer hover:bg-surface-hi ' +
              (editingName === m.name ? 'bg-surface-hi' : '')
            }
            onClick={() => onSelect(m)}
          >
            <span className="truncate text-[12px]">
              {defaultName === m.name ? '★ ' : '  '}
              {m.name}{m.is_legacy ? ' (legacy)' : ''}
            </span>
            {!m.is_legacy && (
              <span className="flex gap-1 shrink-0">
                {defaultName !== m.name && (
                  <button
                    type="button"
                    className="px-1.5 py-0.5 text-[11px] hover:underline"
                    onClick={(e) => { e.stopPropagation(); setDefault(m.name); }}
                    disabled={busy}
                  >
                    设为默认
                  </button>
                )}
                <button
                  type="button"
                  className="px-1.5 py-0.5 text-[11px] text-danger hover:underline"
                  onClick={(e) => { e.stopPropagation(); removeOne(m.name); }}
                  disabled={busy}
                >
                  删
                </button>
              </span>
            )}
          </div>
        ))}
        <div
          className="px-2 py-2 text-[12px] hover:bg-surface-hi cursor-pointer border-t border-border"
          onClick={startNew}
        >
          + 新增模型
        </div>
      </div>

      <div className="space-y-2">
        <div className="text-[12px] font-semibold">{editingName ? '编辑 ' + editingName : '新增模型'}</div>
        <Field label="名字">
          <input
            value={draft.name}
            onChange={(e) => setDraft({ ...draft, name: e.target.value })}
            disabled={!!editingName}
          />
        </Field>
        <Field label="provider">
          <select value={draft.provider} onChange={(e) => setDraft({ ...draft, provider: e.target.value })}>
            <option value="openai">openai</option>
            <option value="copilot">copilot</option>
          </select>
        </Field>
        <Field label="model">
          <input
            value={draft.model}
            onChange={(e) => setDraft({ ...draft, model: e.target.value })}
          />
        </Field>
        {draft.provider === 'openai' && (
          <>
            <Field label="base_url">
              <input
                value={draft.base_url}
                onChange={(e) => setDraft({ ...draft, base_url: e.target.value })}
              />
            </Field>
            <Field label="api_key">
              <input
                type="password"
                value={draft.api_key}
                placeholder={editingName ? '聚焦后输入新 api_key,留空则保留旧值' : ''}
                onFocus={onApiKeyFocus}
                onChange={onApiKeyChange}
                onBlur={onApiKeyBlur}
              />
            </Field>
          </>
        )}
        <button
          type="button"
          className="px-3 py-1 bg-accent text-white rounded disabled:opacity-60"
          onClick={submit}
          disabled={busy}
        >
          {editingName ? '保存' : '新增'}
        </button>
      </div>
    </div>
  );
}

function Field({ label, children }) {
  return (
    <label className="flex items-center gap-2 text-[12px]">
      <span className="w-20 text-right text-fg-mute">{label}</span>
      <span className="flex-1 [&>input]:w-full [&>input]:px-2 [&>input]:py-1 [&>input]:border [&>input]:border-border [&>input]:rounded [&>input]:bg-surface-alt [&>input]:text-fg [&>input]:outline-none [&>select]:px-2 [&>select]:py-1 [&>select]:border [&>select]:border-border [&>select]:rounded [&>select]:bg-surface-alt [&>select]:text-fg">
        {children}
      </span>
    </label>
  );
}
