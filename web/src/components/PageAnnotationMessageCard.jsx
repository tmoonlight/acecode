import { useState } from 'react';
import { VsIcon } from './Icon.jsx';
import { ImageLightbox } from './ImageLightbox.jsx';

export function PageAnnotationMessageCard({ title, comments = [], imageUrl }) {
  const [preview, setPreview] = useState(null);
  const several = comments.length > 1;
  return (
    <>
      <article className="ace-page-annotation-message" data-page-annotation-message="true">
        {imageUrl ? (
          <button
            type="button"
            className="ace-page-annotation-message-shot"
            aria-label={title || undefined}
            onClick={() => setPreview({ src: imageUrl, alt: title || '' })}
          >
            <img src={imageUrl} alt="" />
          </button>
        ) : null}
        {(title || comments.length > 0) ? (
          <div className="ace-page-annotation-message-body">
            {title ? (
              <div className="ace-page-annotation-message-title">
                <span className="ace-page-annotation-message-mark"><VsIcon name="chat" size={11} /></span>
                <span>{title}</span>
              </div>
            ) : null}
            {comments.map((comment) => (
              <p key={comment.number} className="ace-page-annotation-message-text">
                {several ? <span className="ace-page-annotation-message-number">{comment.number}</span> : null}
                {comment.description}
              </p>
            ))}
          </div>
        ) : null}
        <ImageLightbox preview={preview} onClose={() => setPreview(null)} />
      </article>
      {comments.length > 0 ? (
        <span className="ace-page-annotation-message-summary">
          <VsIcon name="chat" size={11} />
          {`${comments.length} 条注释`}
        </span>
      ) : null}
    </>
  );
}
