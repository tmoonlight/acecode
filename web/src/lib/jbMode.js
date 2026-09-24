// Developer-mode JB switch. Default off. The rising edge persists the flag,
// asks for the web/desktop dark theme, and starts one fullscreen heat wave.
// Falling edge only persists off.
export const JB_MODE_DEFAULT = false;

export function initialJbMode(loaded) {
  return loaded === true;
}

export async function applyJbModeTransition({
  previous = JB_MODE_DEFAULT,
  next,
  persist,
  setTheme,
  startHeatWave,
}) {
  const saved = await persist(next === true);
  const enabled = saved === true;
  if (previous !== true && enabled) {
    await setTheme('dark');
    startHeatWave();
  }
  return enabled;
}
