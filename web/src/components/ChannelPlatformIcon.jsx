import { clsx } from '../lib/format.js';
import qq from '../assets/channel-icons/qq.svg';
import weixin from '../assets/channel-icons/weixin.svg';
import feishu from '../assets/channel-icons/feishu.svg';
import dingtalk from '../assets/channel-icons/dingtalk.svg';
import telegram from '../assets/channel-icons/telegram.svg';
import discord from '../assets/channel-icons/discord.svg';
import line from '../assets/channel-icons/line.svg';
import { VsIcon } from './Icon.jsx';

const icons = { qq, weixin, feishu, dingtalk, telegram, discord, line };

// 外部平台插件没有内置品牌图标,用单色聊天气泡。
export function ChannelPlatformIcon({ platform, size = 24, box = 'h-9 w-9' }) {
  const src = icons[platform];
  return (
    <span className={clsx('flex shrink-0 items-center justify-center', box)}>
      {src ? (
        <img data-channel-icon={platform} src={src} width={size} height={size} alt="" aria-hidden="true"
          className={platform === 'feishu' ? 'dark:invert' : undefined} />
      ) : (
        <VsIcon data-channel-icon={platform} name="ChatBubble" size={size} aria-hidden="true" />
      )}
    </span>
  );
}
