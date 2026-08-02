export function resolveTokenBudgetPanelState(
  current,
  geometry,
  mode = 'hover',
) {
  const nextMode = mode === 'click' ? 'click' : 'hover';

  if (!current) {
    return {
      ...geometry,
      mode: nextMode,
    };
  }

  if (nextMode === 'click' && current.mode !== 'click') {
    return {
      ...current,
      mode: 'click',
    };
  }

  return current;
}
