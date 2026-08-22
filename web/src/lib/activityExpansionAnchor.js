function finiteNumber(value, fallback = 0) {
  const number = Number(value);
  return Number.isFinite(number) ? number : fallback;
}

function nonNegativeNumber(value, fallback = 0) {
  return Math.max(0, finiteNumber(value, fallback));
}

export function activityAnchorViewportTop({ containerTop = 0, anchorTop = 0 } = {}) {
  return finiteNumber(anchorTop) - finiteNumber(containerTop);
}

export function scrollTopForPreservedActivityAnchor({
  scrollTop = 0,
  anchorViewportTop = 0,
  currentAnchorViewportTop = anchorViewportTop,
  clientHeight = 0,
  scrollHeight = 0,
} = {}) {
  const currentScrollTop = nonNegativeNumber(scrollTop);
  const requestedScrollTop = currentScrollTop
    + finiteNumber(currentAnchorViewportTop)
    - finiteNumber(anchorViewportTop);
  const maximumScrollTop = Math.max(
    0,
    nonNegativeNumber(scrollHeight) - nonNegativeNumber(clientHeight),
  );
  return Math.min(maximumScrollTop, Math.max(0, requestedScrollTop));
}

export function matchesProgrammaticActivityScroll(
  actualScrollTop,
  programmaticScrollTop,
  tolerance = 1,
) {
  if (programmaticScrollTop === null || programmaticScrollTop === undefined) return false;
  return Math.abs(finiteNumber(actualScrollTop) - finiteNumber(programmaticScrollTop))
    <= nonNegativeNumber(tolerance, 1);
}
