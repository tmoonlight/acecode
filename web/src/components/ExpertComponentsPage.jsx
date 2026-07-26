import { useState } from 'react';
import { api } from '../lib/api.js';
import {
  emptyExpertForm,
  expertFormFromDetail,
} from '../lib/expertComponents.js';
import { ExpertCatalog, useExpertCatalogData } from './ExpertCatalog.jsx';
import { ExpertEditor } from './ExpertEditor.jsx';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

export function ExpertComponentsPage({
  workspaceHash = '',
  recentExpertIds = [],
  onRememberExpert,
  onDispatchToNewTask,
}) {
  const catalog = useExpertCatalogData(workspaceHash);
  const [editor, setEditor] = useState(null);
  const [deleteTarget, setDeleteTarget] = useState(null);
  const [deleting, setDeleting] = useState(false);

  const editExpert = async (expert) => {
    if (!expert.managed_global) return;
    try {
      const detail = await api.getExpert(expert.id, catalog.effectiveWorkspace);
      setEditor({ editing: true, form: expertFormFromDetail(detail) });
    } catch (error) {
      toast({ kind: 'err', text: `读取专家组件失败：${error?.message || ''}` });
    }
  };

  const confirmDelete = async () => {
    if (!deleteTarget || deleting) return;
    setDeleting(true);
    try {
      await api.deleteExpert(deleteTarget.id, catalog.effectiveWorkspace);
      setDeleteTarget(null);
      await catalog.refresh();
      toast({ kind: 'ok', text: `已删除“${deleteTarget.display_name}”` });
    } catch (error) {
      toast({ kind: 'err', text: `删除失败：${error?.message || ''}` });
    } finally {
      setDeleting(false);
    }
  };

  const dispatchToNewTask = async (expert, prompt = '') => {
    if (!expert?.id || !onDispatchToNewTask) return false;
    try {
      const accepted = await onDispatchToNewTask(expert, String(prompt || ''));
      if (accepted === false) return false;
      onRememberExpert?.(expert);
      return true;
    } catch (error) {
      toast({ kind: 'err', text: `派遣失败：${error?.message || ''}` });
      return false;
    }
  };

  return (
    <main data-expert-components-page="true" className="flex-1 min-w-0 overflow-y-auto bg-bg">
      <div className="mx-auto max-w-[1240px] px-4 pb-14 pt-6 sm:px-7 lg:px-8">
        <header className="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
          <div>
            <div className="text-[10px] font-medium text-fg-mute">扩展 / 专家组件</div>
            <h1 className="mt-1 text-[21px] font-semibold tracking-[-0.01em] text-fg">找到适合当前工作的专家</h1>
            <p className="mt-1 text-[12px] text-fg-mute">查看擅长领域后派遣到新任务，或维护自己的专家和专家团。</p>
          </div>
          <div className="flex shrink-0 items-center gap-2">
            <button
              type="button"
              onClick={() => setEditor({ editing: false, form: emptyExpertForm('team') })}
              className="flex h-8 items-center gap-1.5 rounded-md border border-border bg-surface px-3 text-[12px] font-medium text-fg-2 transition hover:bg-surface-hi"
            >
              <VsIcon name="extension" size={14} />
              组建专家团
            </button>
            <button
              type="button"
              onClick={() => setEditor({ editing: false, form: emptyExpertForm('agent') })}
              className="flex h-8 items-center gap-1.5 rounded-md bg-accent px-3 text-[12px] font-medium text-white hover:opacity-90"
            >
              <VsIcon name="add" size={14} />
              新建专家
            </button>
          </div>
        </header>

        <div className="mt-6">
          <ExpertCatalog
            experts={catalog.experts}
            diagnostics={catalog.diagnostics}
            loading={catalog.loading}
            error={catalog.error}
            onRetry={catalog.refresh}
            recentIds={recentExpertIds}
            onDispatch={(expert) => dispatchToNewTask(expert)}
            onOpeningPrompt={dispatchToNewTask}
            onEdit={editExpert}
            onDelete={setDeleteTarget}
          />
        </div>
      </div>

      {editor && (
        <ExpertEditor
          initial={editor.form}
          editing={editor.editing}
          experts={catalog.experts}
          workspaceHash={catalog.effectiveWorkspace}
          onClose={() => setEditor(null)}
          onSaved={async () => {
            const kind = editor.form.type === 'team' ? '专家团' : '专家';
            setEditor(null);
            await catalog.refresh();
            toast({ kind: 'ok', text: `${kind}已保存` });
          }}
        />
      )}

      {deleteTarget && (
        <Modal
          onClose={() => !deleting && setDeleteTarget(null)}
          width={420}
          dismissOnBackdrop={!deleting}
          dismissOnEscape={!deleting}
          labelledBy="expert-delete-title"
        >
          <div data-expert-delete-dialog="true" className="p-5">
            <h2 id="expert-delete-title" className="text-[15px] font-semibold text-fg">
              删除{deleteTarget.type === 'team' ? '专家团' : '专家'}
            </h2>
            <p className="mt-2 text-[12px] leading-5 text-fg-2">
              确定删除“{deleteTarget.display_name}”吗？以前的对话仍会保留。
            </p>
            <div className="mt-5 flex justify-end gap-2">
              <button type="button" disabled={deleting} onClick={() => setDeleteTarget(null)} className="h-8 rounded-md border border-border px-3 text-[12px] text-fg-2 hover:bg-surface-hi disabled:opacity-50">取消</button>
              <button type="button" disabled={deleting} onClick={confirmDelete} className="h-8 rounded-md bg-danger px-3 text-[12px] font-medium text-white hover:opacity-90 disabled:opacity-50">
                {deleting ? '删除中…' : '删除'}
              </button>
            </div>
          </div>
        </Modal>
      )}
    </main>
  );
}
