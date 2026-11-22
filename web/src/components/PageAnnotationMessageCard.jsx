import { useMemo, useState } from 'react';
import { VsIcon } from './Icon.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';
import { annotationScreenshotUrl } from '../lib/pageAnnotationMessage.js';

const VISIBLE_ROWS = 5;

// 已发消息卡片：整屏截屏 + `X 条注释` 标签，明细在悬停浮层里。
// 卡片内没有任何操作按钮——消息是 transcript，能改就等于改历史（PRD §6.7）。
export function PageAnnotationMessageCard({ pages = [], shots = null }) {
  const [preview, setPreview] = useState(null);
  const [hovering, setHovering] = useState(false);
  const [expanded, setExpanded] = useState(false);

  const shotsByPage = useMemo(() => (pages || []).map((page) => {
    const attachment = page.shot && shots ? shots.get(page.shot) : null;
    return attachment ? annotationScreenshotUrl(attachment) : '';
  }), [pages, shots]);

  const total = pages.reduce((sum, page) => sum + page.comments.length, 0);
  if (!total) return null;
  const multiPage = pages.length > 1;
  const visibleRows = pages.reduce((sum, page) => sum + page.comments.length, 0);

  return (
    <article className="ace-page-annotation-message" data-page-annotation-message="true">
      {pages.map((page, index) => {
        const url = shotsByPage[index];
        if (!url) return null;
        return (
          <button key={`${page.shot || index}`} type="button"
            className="ace-page-annotation-message-shot"
            aria-label={`查看${page.title || '页面'}截屏`}
            onClick={() => setPreview({ src: url, alt: page.title || '' })}>
            <img src={url} alt="" />
          </button>
        );
      })}
      <div className="ace-page-annotation-message-footer"
        onPointerEnter={() => setHovering(true)}
        onPointerLeave={() => setHovering(false)}>
        {pages.map((page) => page.title).filter(Boolean).slice(0, 1).map((title, index) => (
          <span key={index} className="ace-page-annotation-message-title">
            <span className="ace-page-annotation-message-mark"><VsIcon name="chat" size={11} /></span>
            <span>{title}</span>
          </span>
        ))}
        <span className="ace-page-annotation-message-summary" tabIndex={0} role="button"
          aria-expanded={hovering}
          onFocus={() => setHovering(true)}
          onBlur={() => setHovering(false)}
          onPointerEnter={() => setHovering(true)}
          onKeyDown={(event) => { if (event.key === 'Escape') setHovering(false); }}>
          <VsIcon name="chat" size={11} />
          {`${total} 条注释`}
        </span>
        {hovering ? (
          <div className="ace-page-annotation-message-detail" role="tooltip"
            style={{ maxHeight: expanded ? '60vh' : undefined }}>
            <div className="ace-page-annotation-message-detail-list"
              style={{ maxHeight: visibleRows > VISIBLE_ROWS && !expanded ? '190px' : undefined }}>
              {pages.map((page, pageIndex) => (
                <div key={`${page.url}-${pageIndex}`} className="ace-page-annotation-message-detail-group">
                  {multiPage && page.title ? (
                    <div className="ace-page-annotation-message-detail-page">{page.title}</div>
                  ) : null}
                  {page.comments.map((comment) => (
                    <div key={`${comment.number}-${comment.description}`}
                      className="ace-page-annotation-message-detail-row">
                      <span className="ace-page-annotation-message-tag">{comment.element || 'div'}</span>
                      <span className="ace-page-annotation-message-detail-text">{comment.description}</span>
                    </div>
                  ))}
                </div>
              ))}
            </div>
            {visibleRows > VISIBLE_ROWS && !expanded ? (
              <button type="button" className="ace-page-annotation-message-more"
                onClick={() => setExpanded(true)}>
                {`还有 ${visibleRows - VISIBLE_ROWS} 条`}
              </button>
            ) : null}
          </div>
        ) : null}
      </div>
      <ImageLightbox preview={preview} onClose={() => setPreview(null)} />
    </article>
  );
}
