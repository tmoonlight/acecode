import { clsx } from '../lib/format.js';
import { GeometricAvatar } from './GeometricAvatar.jsx';
import { PixelAvatar } from './PixelAvatar.jsx';

// 设置 > 外观 > 子代理头像。只影响没绑定专家的子代理(绑定了专家的一直用专家头像)。
// 配置里的值是 geometric / pixel;花瓣是默认,排在前面。
const AVATAR_STYLE_OPTIONS = [
  { key: 'geometric', label: '花瓣' },
  { key: 'pixel', label: '像素小人' },
];

// 两张卡片画的是同一批身份:换风格时看到的是「同样几个子代理换了一种画法」。
// 这五个是挑过的,花瓣风格下正好是五种花瓣、五个颜色。
const PREVIEW_IDENTITIES = [
  'subagent:preview-1',
  'subagent:preview-4',
  'subagent:preview-5',
  'subagent:preview-7',
  'subagent:preview-11',
];

export function SubagentAvatarStyleCards({ avatarStyle = 'geometric', onAvatarStyleChange = () => {} }) {
  return (
    <>
      <h3 id="subagent-avatar-style-label" className="text-[14px] font-semibold mb-3">子代理头像</h3>
      <div className="grid grid-cols-2 gap-3 max-w-md" role="radiogroup" aria-labelledby="subagent-avatar-style-label">
        {AVATAR_STYLE_OPTIONS.map((option) => {
          const active = avatarStyle === option.key;
          return (
            <button
              key={option.key}
              type="button"
              role="radio"
              aria-checked={active}
              data-subagent-avatar-style={option.key}
              onClick={() => onAvatarStyleChange(option.key)}
              className={clsx(
                'relative p-3 rounded-lg border text-left transition',
                active ? 'border-accent border-2 bg-accent-bg' : 'border-border bg-surface hover:border-accent/50',
              )}
            >
              <div className="flex items-center gap-2 mb-2">
                {PREVIEW_IDENTITIES.map((identity) => (option.key === 'geometric'
                  ? <GeometricAvatar key={identity} identity={identity} size={28} />
                  : <PixelAvatar key={identity} identity={identity} size={28} />))}
              </div>
              <div className="text-[13px] font-semibold">{option.label}</div>
              {active && <span className="absolute top-2 right-2 w-2.5 h-2.5 rounded-full bg-accent" />}
            </button>
          );
        })}
      </div>
    </>
  );
}
