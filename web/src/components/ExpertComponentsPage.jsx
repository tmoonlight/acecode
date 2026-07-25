import { useEffect, useState } from 'react';
import { api } from '../lib/api.js';
import {
  emptyExpertForm,
  expertFormFromDetail,
  normalizeTargetSessions,
} from '../lib/expertComponents.js';
import { ExpertCatalog, useExpertCatalogData } from './ExpertCatalog.jsx';
import { ExpertEditor } from './ExpertEditor.jsx';
import { Modal } from './Modal.jsx';
import { toast } from './Toast.jsx';
import { VsIcon } from './Icon.jsx';

function TargetConversationDialog({
  target,
  workspaceHash,
  onClose,
  onDispatched,
}) {
  const [sessions, setSessions] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');
  const [busyId, setBusyId] = useState('');

  const load = async () => {
    setLoading(true);
    setError('');
    try {
      const result = workspaceHash && workspaceHash !== '__local__'
        ? await api.listWorkspaceSessions(workspaceHash)
        : await api.listSessions();
      setSessions(normalizeTargetSessions(result).filter((session) => session.active !== false).slice(0, 20));
    } catch (loadError) {
      setError(loadError?.message || '无法读取最近对话');
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => { load(); }, []); // eslint-disable-line react-hooks/exhaustive-deps

  const dispatch = async (session) => {
    if (busyId) return;
    setBusyId(session.id);
    setError('');
    try {
      await api.setSessionExpert(session.id, target.expert.id);
      if (target.prompt) {
        await api.setSessionDraft(
          session.id,
          target.prompt,
          session.workspace_hash || workspaceHash || '',
        );
      }
      onDispatched(target.expert, session, target.prompt);
    } catch (dispatchError) {
      setError(dispatchError?.message || '派遣失败');
    } finally {
      setBusyId('');
    }
  };

  return (
    <Modal
      onClose={onClose}
      width={560}
      dismissOnBackdrop={!busyId}
      dismissOnEscape={!busyId}
      layerClassName="z-[310]"
      labelledBy="expert-target-title"
    >
      <div data-expert-target-conversation="true" className="flex max-h-[78vh] flex-col">
        <header className="flex items-start justify-between gap-4 border-b border-border px-5 py-4">
          <div>
            <h2 id="expert-target-title" className="text-[15px] font-semibold text-fg">选择目标对话</h2>
            <p className="mt-1 text-[10px] text-fg-mute">
              将“{target.expert.display_name}”派遣到已有对话，不会新建任务。
            </p>
          </div>
          <button type="button" onClick={onClose} disabled={!!busyId} className="p-1 text-fg-mute hover:text-fg disabled:opacity-50" aria-label="关闭">
            <VsIcon name="close" size={16} />
          </button>
        </header>
        <div className="flex-1 overflow-y-auto px-4 py-3">
          {loading ? (
            <div className="flex h-32 items-center justify-center text-[11px] text-fg-mute">
              <span className="ace-spinner mr-2 h-4 w-4" />
              正在读取最近对话…
            </div>
          ) : sessions.length > 0 ? (
            <div className="overflow-hidden rounded-lg border border-border">
              {sessions.map((session) => (
                <button
                  key={session.id}
                  type="button"
                  disabled={!!busyId}
                  onClick={() => dispatch(session)}
                  className="flex min-h-12 w-full items-center gap-3 border-b border-border bg-surface px-3 py-2.5 text-left last:border-b-0 hover:bg-surface-hi disabled:cursor-wait disabled:opacity-50"
                >
                  <VsIcon name="newSession" size={15} className="shrink-0 text-fg-mute" />
                  <span className="min-w-0 flex-1">
                    <span className="block truncate text-[12px] font-medium text-fg">{session.title}</span>
                    <span className="mt-0.5 block truncate text-[9px] text-fg-mute">{session.workspace_name || session.cwd || '最近对话'}</span>
                  </span>
                  {busyId === session.id
                    ? <span className="ace-spinner h-3.5 w-3.5 shrink-0" />
                    : <VsIcon name="expandRight" size={13} className="shrink-0 text-fg-mute" />}
                </button>
              ))}
            </div>
          ) : (
            <div className="flex h-36 flex-col items-center justify-center rounded-lg border border-dashed border-border bg-surface text-center">
              <VsIcon name="newSession" size={22} className="text-fg-mute" />
              <p className="mt-2 text-[12px] text-fg-2">没有可派遣的已有对话</p>
              <p className="mt-1 text-[10px] text-fg-mute">先打开或创建一个对话，再从真实聊天框中选择专家。</p>
            </div>
          )}
          {error && (
            <div className="mt-3 rounded-md border border-danger bg-danger-bg px-3 py-2 text-[10px] text-danger">
              {error}
            </div>
          )}
        </div>
        <footer className="flex justify-end gap-2 border-t border-border px-4 py-3">
          {error && !busyId && (
            <button type="button" onClick={load} className="h-8 rounded-md border border-border px-3 text-[11px] text-fg-2 hover:bg-surface-hi">
              重新加载
            </button>
          )}
          <button type="button" onClick={onClose} disabled={!!busyId} className="h-8 rounded-md border border-border px-3 text-[11px] text-fg-2 hover:bg-surface-hi disabled:opacity-50">
            取消
          </button>
        </footer>
      </div>
    </Modal>
  );
}

export function ExpertComponentsPage({
  workspaceHash = '',
  recentExpertIds = [],
  onRememberExpert,
}) {
  const catalog = useExpertCatalogData(workspaceHash);
  const [editor, setEditor] = useState(null);
  const [deleteTarget, setDeleteTarget] = useState(null);
  const [deleting, setDeleting] = useState(false);
  const [dispatchTarget, setDispatchTarget] = useState(null);

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

  const requestTarget = async (expert, prompt = '') => {
    setDispatchTarget({ expert, prompt });
    return true;
  };

  return (
    <main data-expert-components-page="true" className="flex-1 min-w-0 overflow-y-auto bg-bg">
      <div className="mx-auto max-w-[1240px] px-4 pb-14 pt-6 sm:px-7 lg:px-8">
        <header className="flex flex-col gap-4 sm:flex-row sm:items-start sm:justify-between">
          <div>
            <div className="text-[10px] font-medium text-fg-mute">扩展 / 专家组件</div>
            <h1 className="mt-1 text-[21px] font-semibold tracking-[-0.01em] text-fg">找到适合当前工作的专家</h1>
            <p className="mt-1 text-[12px] text-fg-mute">查看擅长领域后派遣到已有对话，或维护自己的专家和专家团。</p>
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
            onDispatch={(expert) => requestTarget(expert)}
            onOpeningPrompt={requestTarget}
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

      {dispatchTarget && (
        <TargetConversationDialog
          target={dispatchTarget}
          workspaceHash={workspaceHash}
          onClose={() => setDispatchTarget(null)}
          onDispatched={(expert, session, prompt) => {
            onRememberExpert?.(expert);
            setDispatchTarget(null);
            toast({
              kind: 'ok',
              text: prompt
                ? `已派遣到“${session.title}”，开场白已写入该对话草稿`
                : `已派遣到“${session.title}”`,
            });
          }}
        />
      )}
    </main>
  );
}
