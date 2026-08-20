export const SIDEBAR_TITLE_MARQUEE_MIN_OVERFLOW_PX = 1;
export const SIDEBAR_TITLE_MARQUEE_END_FADE_PX = 12;
export const SIDEBAR_TITLE_MARQUEE_SPEED_PX_PER_SECOND = 32;
// The original 15/70/15 timing is scaled to 7.5/70/7.5 of its old duration:
// travel time stays unchanged while each endpoint wait is exactly halved.
export const SIDEBAR_TITLE_MARQUEE_ENDPOINT_HOLD_FRACTION = 3 / 34;
export const SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION = 14 / 17;

const IDLE_METRICS = Object.freeze({
  overflowing: false,
  distancePx: 0,
  durationMs: 0,
});

function renderedWidth(value) {
  const width = Number(value);
  return Number.isFinite(width) && width > 0 ? width : 0;
}

export function sidebarTitleMarqueeMetrics(contentWidth, viewportWidth) {
  const content = renderedWidth(contentWidth);
  const viewport = renderedWidth(viewportWidth);
  if (!content || !viewport) return IDLE_METRICS;

  const overflowPx = Math.max(0, Math.ceil(content - viewport));
  if (overflowPx <= SIDEBAR_TITLE_MARQUEE_MIN_OVERFLOW_PX) return IDLE_METRICS;
  const distancePx = overflowPx + SIDEBAR_TITLE_MARQUEE_END_FADE_PX;

  const travelDurationMs = (
    distancePx / SIDEBAR_TITLE_MARQUEE_SPEED_PX_PER_SECOND
  ) * 1000;
  // Do not clamp duration: any minimum or maximum would make some titles
  // travel faster or slower than the configured visual speed.
  const durationMs = Math.round(
    travelDurationMs / SIDEBAR_TITLE_MARQUEE_TRAVEL_FRACTION,
  );

  return {
    overflowing: true,
    distancePx,
    durationMs,
  };
}
