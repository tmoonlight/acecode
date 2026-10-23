// Pure decisions for the HUD motion layer (components/HudEffects.jsx).

// The daemon restores the saved variant shortly after the first paint. A
// variant change this close to the boot sweep is that restore, not a user
// switch, so it must not play a second animation on top of the first.
export const HUD_SWITCH_GRACE_MS = 1500;

// previous/current: { colorTheme, variant } snapshots while effects are on;
// previous is null when the effects layer was off or not yet shown.
export function hudBurstForChange(previous, current, elapsedSinceBoot = Infinity) {
  if (!current) return null;
  if (!previous || previous.colorTheme !== current.colorTheme) return 'boot';
  if (previous.variant !== current.variant && elapsedSinceBoot >= HUD_SWITCH_GRACE_MS) return 'switch';
  return null;
}

export function hudVariantLabel(options, variantId) {
  return (Array.isArray(options) ? options : []).find((option) => option.id === variantId)?.name || '';
}
