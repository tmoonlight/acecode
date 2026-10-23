// Motion layer for themes with the HUD chrome: a slow scan band, a one-shot
// boot sweep when the theme becomes active, and a glitch interstitial when the
// user switches variants. Purely decorative: aria-hidden, no pointer events,
// unmounted entirely when the theme's effects switch is off.

import { useEffect, useRef, useState } from 'react';
import { hudBurstForChange, hudVariantLabel } from '../lib/hudEffects.js';
import '../styles/themeHud.css';

const BURST_MS = { boot: 1400, switch: 1100 };

// Rendered by ThemeProvider with its resolved theme state (props, not
// useTheme(), so the provider module does not import itself back).
export function HudEffects({ chrome, effects, colorTheme, variant, variantOptions }) {
  const enabled = chrome === 'hud' && effects !== false;
  const [burst, setBurst] = useState(null);
  const previous = useRef(null);
  const bootAt = useRef(0);

  useEffect(() => {
    const current = { colorTheme, variant };
    const now = Date.now();
    const kind = enabled ? hudBurstForChange(previous.current, current, now - bootAt.current) : null;
    previous.current = enabled ? current : null;
    if (!kind) return;
    if (kind === 'boot') bootAt.current = now;
    setBurst({ kind, key: `${kind}:${now}` });
  }, [enabled, colorTheme, variant]);

  // Each burst owns its own timer, so a later theme update cannot cancel the
  // cleanup of an overlay that is already playing.
  useEffect(() => {
    if (!burst) return undefined;
    const timer = setTimeout(() => setBurst(null), BURST_MS[burst.kind]);
    return () => clearTimeout(timer);
  }, [burst]);

  if (!enabled) return null;
  const label = hudVariantLabel(variantOptions, variant);
  // The sweep stays under dialogs; bursts sit above them so a variant switch
  // made from Settings still shows its transition.
  return (
    <>
      <div className="ace-hud-effects" aria-hidden="true">
        <div className="ace-hud-sweep" />
      </div>
      <div className="ace-hud-effects ace-hud-effects-top" aria-hidden="true">
        {burst?.kind === 'boot' && (
          <div key={burst.key} className="ace-hud-burst ace-hud-boot">
            <div className="ace-hud-boot-line" />
            <div className="ace-hud-boot-label">
              <span>LINK ESTABLISHED</span>
              {label && <strong>{label}</strong>}
            </div>
          </div>
        )}
        {burst?.kind === 'switch' && (
          <div key={burst.key} className="ace-hud-burst ace-hud-switch">
            <div className="ace-hud-switch-bars"><i /><i /><i /><i /></div>
            <div className="ace-hud-switch-card">
              <svg className="ace-hud-switch-icon" viewBox="0 0 48 42" aria-hidden="true">
                <path d="M24 2 46 40H2Z" fill="none" stroke="currentColor" strokeWidth="2.5" strokeLinejoin="miter" />
                <path d="M24 14v13M24 31v4" stroke="currentColor" strokeWidth="3" strokeLinecap="square" />
              </svg>
              <span className="ace-hud-switch-kicker">MODE SWITCH</span>
              {label && <strong className="ace-hud-switch-name">{label}</strong>}
            </div>
          </div>
        )}
      </div>
    </>
  );
}
