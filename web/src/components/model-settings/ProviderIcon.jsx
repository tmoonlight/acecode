import { clsx } from '../../lib/format.js';
import {
  providerLogoIsRaster,
  providerLogoPath,
} from '../../lib/providerLogos.js';
import { VsIcon } from '../Icon.jsx';

export function ProviderIcon({ provider, active = false, size = 'md', className = '' }) {
  const logoPath = providerLogoPath(provider);
  const small = size === 'sm';
  const frameClass = clsx(
    'flex shrink-0 items-center justify-center overflow-hidden rounded-full border bg-surface',
    small ? 'h-5 w-5' : 'h-6 w-6',
    active ? 'border-accent-soft text-accent' : 'border-border text-fg-2',
    className,
  );
  const logoClass = small ? 'block h-3.5 w-3.5 object-contain' : 'block h-4 w-4 object-contain';

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
          className={logoClass}
        />
      ) : (
        <span
          className={clsx('ace-provider-logo-mask', logoClass)}
          style={{ '--ace-provider-logo-url': `url("${logoPath}")` }}
        />
      )}
    </span>
  );
}
