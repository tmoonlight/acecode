import { useEffect, useRef, useState } from 'react';
import { characterSprite, stableLook, stableSeed } from '../lib/officeCharacters.js';

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

export function SessionPixelAvatar({ session, size = 24 }) {
  const expert = session?.expert;
  const [failedImage, setFailedImage] = useState('');
  if (!expert?.avatar_identity) return null;
  const image = expert.avatar_url;
  return image && failedImage !== image
    ? <img src={image} alt="" width={size} height={size} className="shrink-0 rounded object-cover"
      onError={() => setFailedImage(image)} />
    : <PixelAvatar identity={expert.avatar_identity} size={size} />;
}
