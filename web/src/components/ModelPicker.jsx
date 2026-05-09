// 顶部 chat header 上的模型下拉。
// 失败回滚 + 红 toast(经 errors.js 做 i18n)。
// 当前值不在 list 时插一条 disabled 灰条提示用户(改名/删除场景)。

import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import { toast } from './Toast.jsx';
import { lookupErrorMessage } from '../lib/errors.js';
import { buildOptionsWithOrphan } from '../lib/modelPicker.js';

export function ModelPicker({ sessionId, currentName = '', apiClient = api }) {
  const [models, setModels] = useState([]);
  const [value, setValue] = useState(currentName);
  const [busy, setBusy] = useState(false);

  useEffect(() => {
    let off = false;
    apiClient.listModels()
      .then((list) => { if (!off) setModels(Array.isArray(list) ? list : []); })
      .catch(() => {});
    return () => { off = true; };
  }, [apiClient]);

  useEffect(() => { setValue(currentName); }, [currentName]);

  const onChange = async (e) => {
    if (!sessionId) { e.preventDefault(); return; }
    const next = e.target.value;
    const prev = value;
    setValue(next); setBusy(true);
    try {
      const result = await apiClient.switchModel(sessionId, next);
      const label = result && result.name
        ? `${result.name} (${result.provider}/${result.model})`
        : next;
      toast({ kind: 'ok', text: '已切换至 ' + label });
    } catch (err) {
      setValue(prev);
      const code = err && err.code;
      const msg = lookupErrorMessage(code, err && err.message);
      toast({ kind: 'err', text: '切换失败:' + msg });
    } finally {
      setBusy(false);
    }
  };

  const options = buildOptionsWithOrphan(value, models);

  return (
    <select
      value={value}
      onChange={onChange}
      disabled={busy || !sessionId}
      className="h-7 px-2 pr-6 text-[12px] rounded-md border border-border bg-surface-alt text-fg outline-none focus:border-accent transition disabled:opacity-60 disabled:cursor-not-allowed"
    >
      {options.length === 0 && <option value="">—</option>}
      {options.map((m) => (
        <option key={m.name} value={m.name} disabled={m.orphan}>
          {m.label || (m.name + (m.is_legacy ? ' (legacy)' : ''))}
        </option>
      ))}
    </select>
  );
}
