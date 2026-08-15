export function nextHomeLogoEffectEnabled(currentEnabled, activeSessionId) {
  if (!currentEnabled) return false;
  return !activeSessionId;
}
