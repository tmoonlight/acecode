import { useEffect, useRef, useState } from 'react';
import { api } from '../lib/api.js';
import { useSlashCommands } from './SlashCommandsContext.jsx';

export function SkillScanPathsSettings({ focusRequest = 0, disabled = false }) {
  const [config, setConfig] = useState(null);
  const [draft, setDraft] = useState('');
  const [busy, setBusy] = useState('loading');
  const [error, setError] = useState('');
  const [saved, setSaved] = useState(false);
  const [reload, setReload] = useState(0);
  const editorRef = useRef(null);
  const focusedRequest = useRef(0);
  const operation = useRef(false);
  const mounted = useRef(false);
  const slashCommands = useSlashCommands();

  useEffect(() => {
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  useEffect(() => {
    let cancelled = false;
    setBusy('loading');
    setError('');
    api.getSkillScanPaths().then((value) => {
      if (cancelled) return;
      setConfig(value);
      setDraft(value.paths.join('\n'));
    }).catch((err) => {
      if (!cancelled) setError('加载 Skill 扫描路径失败：' + (err?.message || ''));
    }).finally(() => { if (!cancelled) setBusy(''); });
    return () => { cancelled = true; };
  }, [reload]);

  useEffect(() => {
    if (!focusRequest || focusedRequest.current === focusRequest || !config || busy || disabled) return;
    focusedRequest.current = focusRequest;
    editorRef.current?.scrollIntoView({ block: 'center' });
    editorRef.current?.focus({ preventScroll: true });
  }, [focusRequest, config, busy, disabled]);

  const paths = draft.split(/\r?\n/u).map((path) => path.trim()).filter(Boolean);
  const dirty = !!config && JSON.stringify(paths) !== JSON.stringify(config.paths);
  const unavailable = disabled || !!busy || !config;
  const save = async (next) => {
    if (operation.current || unavailable) return;
    operation.current = true;
    setBusy('saving'); setError(''); setSaved(false);
    try {
      const value = await api.setSkillScanPaths(next);
      slashCommands.invalidate?.();
      if (!mounted.current) return;
      setConfig(value);
      setDraft(value.paths.join('\n'));
      setSaved(true);
    } catch (err) {
      if (mounted.current) setError('保存 Skill 扫描路径失败：' + (err?.message || ''));
    } finally {
      operation.current = false;
      if (mounted.current) setBusy('');
    }
  };

  return (
    <section className="ace-settings-group" aria-labelledby="skill-scan-paths-heading">
      <h3 id="skill-scan-paths-heading"><label htmlFor="skill-scan-paths">Skill 扫描路径</label></h3>
      <p id="skill-scan-paths-help">每行一个目录，按顺序查找，同名技能优先使用靠前的路径。保存空列表将关闭普通目录扫描。</p>
      <p>{'相对路径从当前工作区逐级向上查找；绝对路径和 ~ 路径作为全局目录。支持 ${ENV} 环境变量。'}</p>
      <p>{'${ACECODE_DATA_DIR} 为数据目录；${XDG_CONFIG_HOME}、${XDG_CACHE_HOME} 默认分别为 ~/.config、~/.cache。未设置的其他变量会跳过。'}</p>
      <textarea ref={editorRef} id="skill-scan-paths" value={draft} rows={8}
        aria-describedby="skill-scan-paths-help" disabled={unavailable} spellCheck={false}
        onChange={(event) => { setDraft(event.target.value); setSaved(false); }}
        className="w-full px-3 py-2.5 text-[13px] rounded-md border border-border bg-surface text-fg outline-none focus:border-accent transition leading-relaxed resize-y disabled:opacity-60" />
      <div className="flex flex-wrap items-center gap-3 mt-2">
        <button type="button" className="ace-settings-button ace-settings-primary" disabled={unavailable || !dirty}
          onClick={() => save(paths)}>{busy === 'saving' ? '保存中...' : '保存路径'}</button>
        <button type="button" className="ace-settings-button" disabled={unavailable || (!config.customized && !dirty)}
          onClick={() => save(null)}>恢复默认</button>
        {config && <span className="text-[12px] text-fg-mute">{config.customized ? '使用自定义路径' : '使用默认路径'}</span>}
        {!config && !busy && <button type="button" className="ace-settings-button" onClick={() => setReload((value) => value + 1)}>重新加载</button>}
      </div>
      <div className="min-h-6 mt-2 text-[12px]" role="status" aria-live="polite">
        {error ? <span className="text-danger">{error}</span> : busy === 'loading' ? '加载中...' : saved ? <span className="text-fg-mute">已保存，技能扫描路径已更新</span> : null}
      </div>
    </section>
  );
}
