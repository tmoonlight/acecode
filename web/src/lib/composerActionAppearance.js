import { interfaceIconSvg } from './interfaceIcons.js';

export const COMPOSER_ACTION_BUTTON_CLASS = 'ace-composer-send w-7 h-7 shrink-0 rounded-full flex items-center justify-center transition bg-accent text-white enabled:hover:opacity-90 focus-visible:outline-2 focus-visible:outline-offset-2 focus-visible:outline-accent';
export const COMPOSER_ACTION_ICON_SIZE = 14;

// The desktop surface receives the same control definition and resolved theme,
// including installed themes. Measuring an enabled probe also works while the
// main composer/window is hidden or currently displays its stop control.
export function readComposerSendAppearance(doc = document, env = window) {
  const probe = doc.createElement('button');
  probe.type = 'button'; probe.className = COMPOSER_ACTION_BUTTON_CLASS;
  probe.style.cssText = 'position:fixed;left:-10000px;top:0;visibility:hidden;pointer-events:none;transition:none';
  probe.innerHTML = interfaceIconSvg('Send', COMPOSER_ACTION_ICON_SIZE);
  doc.body.append(probe);
  try {
    const style = env.getComputedStyle(probe);
    const enabled = { size: parseFloat(style.height) || 28, iconSize: COMPOSER_ACTION_ICON_SIZE,
      background: style.backgroundColor, backgroundImage: style.backgroundImage, color: style.color,
      borderRadius: style.borderRadius, boxShadow: style.boxShadow,
      iconSvg: interfaceIconSvg('Send', COMPOSER_ACTION_ICON_SIZE), pixelRatio: env.devicePixelRatio || 1 };
    probe.disabled = true;
    const disabled = env.getComputedStyle(probe);
    return { ...enabled, disabledBackground: disabled.backgroundColor, disabledColor: disabled.color, disabledOpacity: disabled.opacity };
  } finally { probe.remove(); }
}
