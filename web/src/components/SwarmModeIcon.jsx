const HEX_CELLS = [
  [12, 12],
  [12, 7.4],
  [16, 9.7],
  [16, 14.3],
  [12, 16.6],
  [8, 14.3],
  [8, 9.7],
];

function hexagonPoints(cx, cy, radius = 2.55) {
  return Array.from({ length: 6 }, (_, index) => {
    const angle = (Math.PI / 3) * index;
    return `${cx + radius * Math.cos(angle)},${cy + radius * Math.sin(angle)}`;
  }).join(' ');
}

export function SwarmModeIcon({ size = 16, className = '' }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      aria-hidden="true"
      className={className}
    >
      {HEX_CELLS.map(([cx, cy], index) => (
        <polygon
          key={`${cx}-${cy}`}
          points={hexagonPoints(cx, cy)}
          fill={index === 0 ? 'currentColor' : 'none'}
          fillOpacity={index === 0 ? 0.14 : 0}
          stroke="currentColor"
          strokeWidth="1.05"
          strokeLinejoin="round"
          vectorEffect="non-scaling-stroke"
        />
      ))}
    </svg>
  );
}
