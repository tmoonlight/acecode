// 路径选择器底栏「新建文件夹」弹出的小窗:一个名称输入框 + 取消 / 确定 + 右上角关闭。
// 名称校验与真正的创建都由 PathPickerModal 的 onCreate 完成:返回非空字符串表示失败原因
// (显示在输入框下方,弹窗保持打开),返回 '' 表示已建好,由调用方关掉弹窗。
//
// 作为 PathPickerModal 的兄弟节点渲染而不是放进它的子树:React 合成事件会穿过 portal 冒泡,
// 放进去的话输入框里的 Alt+← 会被选择器的「后退」快捷键接走。
import { useState } from 'react';
import { Modal } from './Modal.jsx';
import { VsIcon } from './Icon.jsx';
import { NEW_FOLDER_NAME_MAX_LENGTH } from '../lib/pathPicker.js';

export function PathPickerNewFolderDialog({ onCreate, onCancel }) {
  const [name, setName] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const cancel = () => {
    if (!busy) onCancel?.();
  };

  const submit = async (event) => {
    event.preventDefault();
    if (busy || !name.trim()) return;
    setBusy(true);
    setError('');
    let problem = '';
    try {
      problem = await onCreate?.(name);
    } finally {
      setBusy(false);
    }
    if (problem) setError(problem);
  };

  return (
    <Modal
      onClose={cancel}
      width={400}
      dismissOnBackdrop={false}
      dismissOnEscape={!busy}
      labelledBy="ace-path-picker-new-folder-title"
    >
      {() => (
        <form onSubmit={submit} className="ace-modal-form">
          <div className="shrink-0 pl-5 pr-3 pt-3.5 pb-2 flex items-center gap-3">
            <h2 id="ace-path-picker-new-folder-title" className="flex-1 min-w-0 text-[15px] font-semibold text-fg">
              新建文件夹
            </h2>
            <button
              type="button"
              className="ace-path-picker-icon-btn"
              onClick={cancel}
              disabled={busy}
              aria-label="关闭"
              title="关闭"
            >
              <VsIcon name="close" size={15} />
            </button>
          </div>

          <div className="px-5 pt-1 pb-4">
            <input
              value={name}
              onChange={(event) => {
                setName(event.target.value);
                setError('');
              }}
              readOnly={busy}
              maxLength={NEW_FOLDER_NAME_MAX_LENGTH}
              autoComplete="off"
              spellCheck={false}
              placeholder="请输入文件夹名称"
              aria-label="文件夹名称"
              aria-invalid={error ? 'true' : undefined}
              aria-describedby={error ? 'ace-path-picker-new-folder-error' : undefined}
              className="w-full h-9 px-3 rounded-lg border border-border bg-bg text-[13px] text-fg outline-none focus:border-accent focus:ring-1 focus:ring-accent placeholder:text-fg-mute/60"
            />
            {error && (
              <div
                id="ace-path-picker-new-folder-error"
                role="alert"
                className="mt-2 text-[12px] leading-5 text-danger"
              >
                {error}
              </div>
            )}
          </div>

          <div className="shrink-0 px-5 py-3 border-t border-border bg-surface-alt flex items-center justify-end gap-2">
            <button type="button" onClick={cancel} disabled={busy} className="ace-path-picker-soft-btn">
              取消
            </button>
            <button
              type="submit"
              data-ace-dialog-primary="true"
              disabled={busy || !name.trim()}
              className="h-8 min-w-[82px] px-3 rounded-lg bg-accent text-white text-[12px] font-medium hover:brightness-110 disabled:opacity-50 disabled:hover:brightness-100 flex items-center justify-center gap-1.5"
            >
              {busy && <span className="ace-spinner w-3 h-3" />}
              确定
            </button>
          </div>
        </form>
      )}
    </Modal>
  );
}
