export const MAX_LIGHT_DISTANCE_PX = 80;
export const IDLE_LOGO_LIGHT_RADIUS_PX = 42;

export function clampLightToRadius(
  lightX,
  lightY,
  centerX,
  centerY,
  maximumDistance = MAX_LIGHT_DISTANCE_PX,
) {
  const offsetX = lightX - centerX;
  const offsetY = lightY - centerY;
  const distance = Math.hypot(offsetX, offsetY);

  if (!Number.isFinite(distance) || distance === 0 || distance <= maximumDistance) {
    return { x: lightX, y: lightY };
  }

  const scale = maximumDistance / distance;
  return {
    x: centerX + offsetX * scale,
    y: centerY + offsetY * scale,
  };
}

export function createRandomLogoLightOffset(
  random = Math.random,
  maximumDistance = IDLE_LOGO_LIGHT_RADIUS_PX,
) {
  const angleSample = Number(random());
  const distanceSample = Number(random());
  const normalizedAngle = Number.isFinite(angleSample)
    ? Math.min(Math.max(angleSample, 0), 1)
    : 0;
  const normalizedDistance = Number.isFinite(distanceSample)
    ? Math.min(Math.max(distanceSample, 0), 1)
    : 0;
  const safeMaximumDistance = Number.isFinite(maximumDistance)
    ? Math.max(maximumDistance, 0)
    : 0;
  const angle = normalizedAngle * Math.PI * 2;
  const distance = Math.sqrt(normalizedDistance) * safeMaximumDistance;

  return {
    x: Math.cos(angle) * distance,
    y: Math.sin(angle) * distance,
  };
}

export function interpolateLogoLightOffset(start, target, progress) {
  const normalizedProgress = Number.isFinite(progress)
    ? Math.min(Math.max(progress, 0), 1)
    : 0;
  const easedProgress = normalizedProgress
    * normalizedProgress
    * (3 - 2 * normalizedProgress);

  return {
    x: start.x + (target.x - start.x) * easedProgress,
    y: start.y + (target.y - start.y) * easedProgress,
  };
}
