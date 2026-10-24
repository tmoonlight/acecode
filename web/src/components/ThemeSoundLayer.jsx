// Sound layer for themes that declare a sound style. Rendered by ThemeProvider
// with its resolved state (props, like HudEffects): configures the sound
// module, listens for clicks while sounds are on, and plays the boot / mode
// switch sounds at the same moments as the HUD motion bursts.

import { useEffect, useLayoutEffect, useRef } from 'react';
import { hudBurstForChange } from '../lib/hudEffects.js';
import { configureThemeSounds, installThemeSoundInteractions, playThemeSound } from '../lib/themeSounds.js';

export function ThemeSoundLayer({ style, muted, colorTheme, variant }) {
  const active = !!style && !muted;
  const previous = useRef(null);
  const bootAt = useRef(0);

  useLayoutEffect(() => {
    configureThemeSounds({ style, muted });
  }, [style, muted]);
  useEffect(() => () => configureThemeSounds({ style: null }), []);

  useEffect(() => (active ? installThemeSoundInteractions(globalThis.document) : undefined), [active]);

  // Tracked even while muted, so unmuting does not replay the boot sound.
  useEffect(() => {
    if (!style) {
      previous.current = null;
      return;
    }
    const current = { colorTheme, variant };
    const now = Date.now();
    const kind = hudBurstForChange(previous.current, current, now - bootAt.current);
    previous.current = current;
    if (!kind) return;
    if (kind === 'boot') bootAt.current = now;
    playThemeSound(kind);
  }, [style, colorTheme, variant]);

  return null;
}
