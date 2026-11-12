// 子代理的花瓣头像(设置 > 外观 > 子代理头像的默认项,配置值 geometric):一片「花瓣」绕中心转一圈拼成一朵,
// 每朵一个颜色。长什么样完全由身份字符串(subagentAvatarIdentity)决定,所以同一个子代理在
// 对话、摘要面板、后台任务页签里是同一朵。DOM 在 GeometricAvatar.jsx,这里只算形状。
//
// 头像只有 16px,靠得住的区分只有颜色和轮廓(几瓣、粗瓣还是细条),所以只用纯色、深浅两档,
// 不画描边和渐变;颜色是手工挑的 8 个,没有红色(红色留给出错)和灰色(灰色留给已中止)。

export const GEOMETRIC_AVATAR_HUES = Object.freeze([
  Object.freeze({ key: 'purple', main: '#7F77DD', soft: '#AFA9EC' }),
  Object.freeze({ key: 'blue', main: '#378ADD', soft: '#85B7EB' }),
  Object.freeze({ key: 'cyan', main: '#1FA3C2', soft: '#7FD0E2' }),
  Object.freeze({ key: 'teal', main: '#1D9E75', soft: '#5DCAA5' }),
  Object.freeze({ key: 'green', main: '#6FA82A', soft: '#A9D46B' }),
  Object.freeze({ key: 'amber', main: '#EF9F27', soft: '#FAC775' }),
  Object.freeze({ key: 'coral', main: '#E0603A', soft: '#F2A083' }),
  Object.freeze({ key: 'pink', main: '#D4537E', soft: '#ED93B1' }),
]);

// 花瓣的画法:画在以中心为原点、边长 16 的格子里,尖端朝上;整朵由它旋转 count 次得到。
export const GEOMETRIC_AVATAR_FORMS = Object.freeze([
  Object.freeze({ key: 'circle', path: 'M0,-6.9A2.7,2.7 0 1 1 0,-1.5A2.7,2.7 0 1 1 0,-6.9Z' }),
  Object.freeze({ key: 'leaf', path: 'M0,-7.5Q2.9,-3.8 0,-0.6Q-2.9,-3.8 0,-7.5Z' }),
  Object.freeze({ key: 'spoke', path: 'M-1,-6.3A1,1 0 0 1 1,-6.3V-2.1A1,1 0 0 1 -1,-2.1Z' }),
  Object.freeze({ key: 'pinwheel', path: 'M0,0L0,-7.3L4.2,-3.4Z' }),
  Object.freeze({ key: 'diamond', path: 'M0,-7.4L2.5,-4.7L0,-2L-2.5,-4.7Z' }),
]);

export const GEOMETRIC_AVATAR_COUNTS = Object.freeze([3, 4, 5, 6, 8]);

// FNV-1a 之后再搅一遍(murmur3 的收尾步骤):只差一个字符的 id 也会落到完全不同的花上。
function hashIdentity(identity) {
  let hash = 2166136261;
  for (const char of String(identity)) hash = Math.imul(hash ^ char.codePointAt(0), 16777619);
  hash ^= hash >>> 16;
  hash = Math.imul(hash, 0x85ebca6b);
  hash ^= hash >>> 13;
  hash = Math.imul(hash, 0xc2b2ae35);
  hash ^= hash >>> 16;
  return hash >>> 0;
}

// 身份 → 这朵花的五个参数:花瓣形状、瓣数、颜色、有没有花心、要不要转半格
// (四根辐条是「+」还是「×」,三片叶子尖朝上还是朝下)。共 5 × 5 × 8 × 2 × 2 = 800 种。
export function geometricAvatarSpec(identity) {
  let rest = hashIdentity(identity);
  const take = (size) => {
    const value = rest % size;
    rest = Math.floor(rest / size);
    return value;
  };
  const form = GEOMETRIC_AVATAR_FORMS[take(GEOMETRIC_AVATAR_FORMS.length)];
  const count = GEOMETRIC_AVATAR_COUNTS[take(GEOMETRIC_AVATAR_COUNTS.length)];
  const hue = GEOMETRIC_AVATAR_HUES[take(GEOMETRIC_AVATAR_HUES.length)];
  const center = take(2) === 1;
  const turned = take(2) === 1;
  return { form: form.key, path: form.path, count, hue: hue.key, main: hue.main, soft: hue.soft, center, turned };
}

// 每片花瓣转多少度、用深色还是浅色。偶数瓣深浅交替;奇数瓣交替不成一圈,全用深色。
export function geometricAvatarPetals(spec) {
  const count = Math.max(1, Number(spec?.count) || 0);
  const step = 360 / count;
  const offset = spec?.turned ? step / 2 : 0;
  return Array.from({ length: count }, (_, index) => ({
    rotate: Math.round((offset + step * index) * 100) / 100,
    tone: count % 2 === 0 && index % 2 === 1 ? 'soft' : 'main',
  }));
}

// 花心用和它压着的花瓣不同的那一档,否则看不出来。
export function geometricAvatarCenterTone(spec) {
  return Number(spec?.count) % 2 === 0 ? 'main' : 'soft';
}
