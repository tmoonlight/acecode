import { createContext, useContext } from 'react';
import { clsx } from '../lib/format.js';
import {
  geometricAvatarCenterTone,
  geometricAvatarPetals,
  geometricAvatarSpec,
} from '../lib/geometricAvatar.js';

// 设置 > 外观 > 子代理头像。App 在最外层给值;没有 Provider 时(单独渲染的组件、测试)按默认的花瓣。
const SubagentAvatarStyleContext = createContext('geometric');
export const SubagentAvatarStyleProvider = SubagentAvatarStyleContext.Provider;
export function useSubagentAvatarStyle() {
  return useContext(SubagentAvatarStyleContext);
}

// 花瓣头像(配置值 geometric):矢量图,任何缩放比例都清楚。状态不用角标:运行中整朵慢慢转,结束后停住,
// 被中止的变灰。
export function GeometricAvatar({ identity, size = 16, status = '' }) {
  const spec = geometricAvatarSpec(identity);
  const stopped = status === 'stopped';
  const fill = (tone) => (stopped ? 'currentColor' : spec[tone]);
  const opacity = (tone) => (stopped && tone === 'soft' ? 0.5 : 0.9);
  const centerTone = geometricAvatarCenterTone(spec);
  return (
    <svg width={size} height={size} viewBox="-8 -8 16 16" aria-hidden="true"
      data-geometric-avatar={identity}
      className={clsx(
        'shrink-0',
        stopped && 'text-fg-mute',
        status === 'running' && 'ace-geometric-avatar-running',
      )}>
      {geometricAvatarPetals(spec).map((petal) => (
        <path key={petal.rotate} d={spec.path} transform={`rotate(${petal.rotate})`}
          fill={fill(petal.tone)} opacity={opacity(petal.tone)} />
      ))}
      {spec.center && <circle r="1.7" fill={fill(centerTone)} opacity={opacity(centerTone)} />}
    </svg>
  );
}
