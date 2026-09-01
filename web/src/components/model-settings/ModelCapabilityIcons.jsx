import {
  modelCapabilityPresentation,
  normalizeModelCapabilities,
} from '../../lib/modelManager.js';
import { VsIcon } from '../Icon.jsx';

export function ModelCapabilityIcons({ capabilities = [] }) {
  const normalized = normalizeModelCapabilities(capabilities);
  if (normalized.length === 0) return null;

  return (
    <span
      className="inline-flex shrink-0 items-center gap-0.5"
      data-model-capabilities="true"
    >
      {normalized.map((capability) => {
        const presentation = modelCapabilityPresentation(capability);
        const accessibleLabel = presentation.known
          ? `${presentation.label}能力`
          : `能力：${presentation.label}`;
        return (
          <span
            key={capability}
            role="img"
            aria-label={accessibleLabel}
            title={accessibleLabel}
            data-model-capability={capability}
            className={`model-capability-icon inline-flex h-5 w-5 items-center justify-center rounded-sm border bg-transparent ${presentation.colorClass}`}
          >
            <VsIcon name={presentation.icon} size={13} />
          </span>
        );
      })}
    </span>
  );
}
