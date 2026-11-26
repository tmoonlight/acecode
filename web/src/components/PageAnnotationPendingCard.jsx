import { useState } from 'react';
import { VsIcon } from './Icon.jsx';

// 待发批注卡片：输入框正上方，按页面分组。会话级视图——已关闭页签的记录
// 也在里面（组头标注「页面已关闭」），它们仍能编辑文本、删除、发送（PRD §6.4）。
export function PageAnnotationPendingCard({ pages, onEdit, onDelete, onRestore }) {
  // 删除立即生效；只保留最近一条可撤销，连删两条时第一条不再回来。
  const [lastRemoved, setLastRemoved] = useState(null);
  const groups = (pages || []).filter((page) => (page.records || []).length > 0);

  const total = groups.reduce((sum, page) => sum + page.records.length, 0);
  if (!total && !lastRemoved) return null;

  const remove = (record) => {
    setLastRemoved(record);
    onDelete?.(record);
  };

  const undo = () => {
    const record = lastRemoved;
    setLastRemoved(null);
    onRestore?.(record);
  };

  return (
    <section className="ace-annotation-pending" aria-label={`待发批注 ${total} 条`}
      data-page-annotation-pending="true">
      <header className="ace-annotation-pending-head">
        <span>待发批注</span>
        <span className="ace-annotation-pending-count">{`${total} 条`}</span>
        {lastRemoved ? (
          <span className="ace-annotation-pending-undo" role="status">
            已删除
            <button type="button" onClick={undo}>撤销</button>
          </span>
        ) : null}
      </header>
      <div className="ace-annotation-pending-body">
        {groups.map((page) => (
          <div key={page.id} className="ace-annotation-pending-group">
            <div className="ace-annotation-pending-page">
              <span className="truncate">{page.title || page.url || '未命名页面'}</span>
              {page.closed ? <span className="ace-annotation-pending-closed">页面已关闭</span> : null}
            </div>
            {page.records.map((record) => (
              <div key={record.id} className="ace-annotation-pending-row">
                {record.thumbnail ? (
                  <img className="ace-annotation-pending-thumb" src={record.thumbnail} alt="" />
                ) : (
                  <span className="ace-annotation-pending-thumb is-empty" aria-hidden="true">
                    <VsIcon name="Inspect" size={12} />
                  </span>
                )}
                <span className="ace-annotation-pending-tag">{record.element}</span>
                <span className="ace-annotation-pending-text">
                  {record.description || '（未填写描述）'}
                </span>
                <button type="button" className="ace-annotation-pending-action"
                  aria-label={`编辑第 ${record.number} 条批注`}
                  onClick={() => onEdit?.(record)}>
                  <PencilIcon />
                </button>
                <button type="button" className="ace-annotation-pending-action"
                  aria-label={`删除第 ${record.number} 条批注`}
                  onClick={() => remove(record)}>
                  <TrashIcon />
                </button>
              </div>
            ))}
          </div>
        ))}
      </div>
    </section>
  );
}

function PencilIcon() {
  return (
    <svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true">
      <path d="M10.8 2.6 13.4 5.2 5.6 13H3v-2.6l7.8-7.8Z" fill="none" stroke="currentColor"
        strokeWidth="1.3" strokeLinejoin="round" />
    </svg>
  );
}

function TrashIcon() {
  return (
    <svg viewBox="0 0 16 16" width="13" height="13" aria-hidden="true">
      <path d="M3.4 4.6h9.2M6.3 4.5V3.3h3.4v1.2M4.7 4.6l.45 8.1h5.7l.45-8.1"
        fill="none" stroke="currentColor" strokeWidth="1.3" strokeLinejoin="round" strokeLinecap="round" />
    </svg>
  );
}
