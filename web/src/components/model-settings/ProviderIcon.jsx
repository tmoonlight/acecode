import { clsx } from '../../lib/format.js';
import {
  providerLogoIsRaster,
  providerLogoPath,
} from '../../lib/providerLogos.js';
import { VsIcon } from '../Icon.jsx';

export function ProviderIcon({ provider, active = false }) {
  const logoPath = providerLogoPath(provider);
  const frameClass = clsx(
    'flex h-6 w-6 shrink-0 items-center justify-center overflow-hidden rounded-full border bg-surface',
    active ? 'border-accent-soft text-accent' : 'border-border text-fg-2',
  );

  if (!logoPath) {
    return (
      <span className={frameClass} aria-hidden="true">
        <VsIcon name="world" size={13} />
      </span>
    );
  }

  return (
    <span className={frameClass} aria-hidden="true">
      {providerLogoIsRaster(provider) ? (
        <img
          src={logoPath}
          alt=""
          width="16"
          height="16"
          loading="lazy"
          decoding="async"
          draggable="false"
          className="block h-4 w-4 object-contain"
        />
      ) : (
        <span
          className="ace-provider-logo-mask block h-4 w-4"
          style={{ '--ace-provider-logo-url': `url("${logoPath}")` }}
        />
      )}
    </span>
  );
}
