import { normalizeSelectionAnnotations } from '../lib/selectionChatContext.js';

export function SelectionAnnotationBadge({ annotations = [], compact = false }) {
  const items = normalizeSelectionAnnotations(annotations);
  if (items.length === 0) return null;
  const label = `${items.length} 条批注`;
  return (
    <span
      className="ace-selection-annotation-badge"
      data-compact={compact ? 'true' : 'false'}
      tabIndex={0}
      aria-label={label}
    >
      <span className="ace-selection-annotation-badge-count">{items.length}</span>
      <span className="ace-selection-annotation-tooltip" role="tooltip">
        <span className="ace-selection-annotation-tooltip-title">{label}</span>
        {items.map((annotation, index) => (
          <span className="ace-selection-annotation-tooltip-item" key={annotation.id || index}>
            <span>{index + 1}</span>
            <span>{annotation.text}</span>
          </span>
        ))}
      </span>
    </span>
  );
}
