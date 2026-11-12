import { useEffect, useRef, useState } from 'react';
import {
  avatarNodOffsetPx,
  avatarNods,
  avatarStatusMark,
  avatarStatusSprites,
  avatarStatusTick,
  subscribeAvatarStatusClock,
} from '../lib/avatarStatus.js';
import { clsx } from '../lib/format.js';
import {
  characterSprite,
  miniSprite,
  stableLook,
  stableSeed,
  subagentAvatarIdentity,
} from '../lib/officeCharacters.js';
import { GeometricAvatar, useSubagentAvatarStyle } from './GeometricAvatar.jsx';

export function PixelAvatar({ identity, size = 36 }) {
  const ref = useRef(null);
  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return;
    const sprite = characterSprite(stableLook(stableSeed(identity)));
    const context = canvas.getContext('2d');
    context.clearRect(0, 0, 14, 14);
    context.drawImage(sprite.cv, 0, 0, 14, 14, 0, 0, 14, 14);
  }, [identity]);
  return <canvas ref={ref} width={14} height={14} aria-hidden="true"
    data-pixel-avatar={identity} className="shrink-0"
    style={{ width: size, height: size, imageRendering: 'pixelated' }} />;
}

// 一排子代理头像(对话里「调用了 N 个智能体」行首);avatars 来自 subagentGroupAvatars。
export function SubagentAvatars({ avatars, size = 16 }) {
  return avatars.map((avatar) => (
    <SubagentAvatar key={avatar.sessionId} sessionId={avatar.sessionId} expert={avatar.expert}
      status={avatar.status} size={size} />
  ));
}

// 子代理头像:绑定了专家用专家的头像(有自定义图片就用图片),否则按子会话 id 生成——
// 设置 > 外观 > 子代理头像决定生成的是花瓣(默认,GeometricAvatar,状态靠转 / 停 / 变灰)还是像素小人。
// 像素小人和专家头像由 status(subagentAvatarStatus)决定右上角的状态图标,和桌面像素办公室
// 同一套:运行中三个点轮流跳、小人隔一会儿点一下头,结束是一颗星,被中止是红色感叹号;
// 不给就只有头像。
export function SubagentAvatar({ sessionId = '', expert = null, status = '', size = 16, className = '' }) {
  const [failedImage, setFailedImage] = useState('');
  const bodyRef = useRef(null);
  const markRef = useRef(null);
  const identity = subagentAvatarIdentity(sessionId, expert);
  const geometric = useSubagentAvatarStyle() === 'geometric' && !expert?.avatar_identity;
  const mark = geometric ? null : avatarStatusMark(status);
  useEffect(() => {
    const body = bodyRef.current;
    const canvas = markRef.current;
    if (body) body.style.transform = '';
    if (!body || !canvas) return undefined;
    const seed = stableSeed(identity);
    const paint = (tick) => {
      const context = canvas.getContext('2d');
      context.clearRect(0, 0, canvas.width, canvas.height);
      for (const sprite of avatarStatusSprites(status, tick)) {
        context.drawImage(miniSprite(sprite.name, sprite.color).cv, sprite.x, sprite.y);
      }
      body.style.transform = avatarNods(status, tick, seed)
        ? `translateY(${avatarNodOffsetPx(size, window.devicePixelRatio)}px)` : '';
    };
    paint(avatarStatusTick());
    // 只有运行中的头像跟着钟走;结束后的图标画一次就不动了。
    return status === 'running' ? subscribeAvatarStatusClock(paint) : undefined;
  }, [identity, status, size, geometric]);
  if (!identity) return null;
  if (geometric) {
    return (
      <span className={clsx('relative inline-flex shrink-0', className)}
        style={{ width: size, height: size }} data-subagent-avatar={status || 'plain'}>
        <GeometricAvatar identity={identity} size={size} status={status} />
      </span>
    );
  }
  const image = expert?.avatar_identity ? expert.avatar_url : '';
  return (
    <span className={clsx('relative inline-flex shrink-0', className)}
      style={{ width: size, height: size }} data-subagent-avatar={status || 'plain'}>
      <span ref={bodyRef} className="inline-flex">
        {image && failedImage !== image
          ? <img src={image} alt="" width={size} height={size} className="shrink-0 rounded object-cover"
            onError={() => setFailedImage(image)} />
          : <PixelAvatar identity={identity} size={size} />}
      </span>
      {mark && (
        <canvas ref={markRef} width={mark.width} height={mark.height} aria-hidden="true"
          data-avatar-status={status} className="pointer-events-none absolute"
          style={{ width: mark.width, height: mark.height, right: mark.right, top: mark.top, imageRendering: 'pixelated' }} />
      )}
    </span>
  );
}
