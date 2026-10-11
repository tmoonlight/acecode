// Shared original Agent Office pixel art. Used by the native pet and React portraits.
// Classic script for file:// previews; importing it as a module installs the same API.
(function (root) {
'use strict';
const INK = '#2b2737';
const mk = (w, h) => { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; };
const rng = s => () => { s |= 0; s = s + 0x6D2B79F5 | 0; let t = Math.imul(s ^ s >>> 15, 1 | s); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; };
const toRGB = h => { const n = parseInt(h.slice(1), 16); return [n >> 16 & 255, n >> 8 & 255, n & 255]; };
const toHex = a => '#' + a.map(v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, '0')).join('');
const mix = (a, b, t) => { const A = toRGB(a), B = toRGB(b); return toHex(A.map((v, i) => v + (B[i] - v) * t)); };
const lite = (c, t) => mix(c, '#ffffff', t), dark = (c, t) => mix(c, '#000000', t);
function outline(cv, col = INK) {
  const g = cv.getContext('2d'), W = cv.width, H = cv.height;
  const im = g.getImageData(0, 0, W, H), d = im.data, a = new Uint8Array(W * H);
  for (let i = 0; i < W * H; i++) a[i] = d[i * 4 + 3] > 0 ? 1 : 0;
  const [r, gg, b] = toRGB(col);
  for (let y = 0; y < H; y++) for (let x = 0; x < W; x++) {
    const i = y * W + x; if (a[i]) continue;
    if ((x > 0 && a[i - 1]) || (x < W - 1 && a[i + 1]) || (y > 0 && a[i - W]) || (y < H - 1 && a[i + W])) {
      const k = i * 4; d[k] = r; d[k + 1] = gg; d[k + 2] = b; d[k + 3] = 255;
    }
  }
  g.putImageData(im, 0, 0);
}
function pix(rows, pal, flip = false, ol = true) {
  const h = rows.length, w = rows[0].length, m = ol ? 1 : 0;
  const cv = mk(w + 2 * m, h + 2 * m), g = cv.getContext('2d');
  rows.forEach((row, y) => { for (let x = 0; x < w; x++) { const c = pal[row[x]]; if (!c) continue; g.fillStyle = c; g.fillRect((flip ? w - 1 - x : x) + m, y + m, 1, 1); } });
  if (ol) outline(cv, pal.ink || INK);
  return cv;
}
const patch = (rows, ps) => {
  if (!ps) return rows; rows = rows.slice();
  for (const [i, s] of ps) rows[i] = [...rows[i]].map((ch, k) => (s[k] && s[k] !== '_') ? s[k] : ch).join('');
  return rows;
};
const SPR = new Map();
const sprite = (key, build) => { let s = SPR.get(key); if (!s) { s = build(); SPR.set(key, s); } return s; };
const BODY = {
  front: [ // 面朝右下（看得见脸）
    '....hhhhh...',
    '..hhhhhhhhh.',
    '.hhhhhhhllhh',
    '.hhhhhhhhhhh',
    '.hhhhhhshhsh',
    '.hhhhsssssss',
    '.Hhhssessses',
    '.Hhhssessses',
    '..Hhsbssssbs',
    '...HSsssssS.',
    '.....sss....',
    '...cwwwwcc..',
    '..ccccwwcccc',
    '.ccccccccccc',
    '.sCccccccccs',
    '..CCCCCCCC..',
    '..pppppppp..'],
  back: [  // 背对镜头（面朝左上）
    '...hhhhhh...',
    '..hhhhhhhhh.',
    '.hhhhllhhhhh',
    '.hhhhhhhhhhh',
    '.hhhhhhhhhhh',
    '.hhhhhhhhhhh',
    '.hhhhhhhhhhs',
    '.HhhhhhhhhHs',
    '..HHhhhhhHS.',
    '...HHHHHH...',
    '.....sss....',
    '...cccccc...',
    '..cccccccc..',
    '.cccccccccc.',
    '.sCccccccCs.',
    '..CCCCCCCC..',
    '..pppppppp..'],
};
const FRAMES = { // 动作 = 在同一模板上打几行补丁（_ 表示不变）
  front: {
    typeA: [[13, '_s__________'], [14, '_C__________']],
    typeB: [[13, '___________s'], [14, '___________c']],
    blink: [[6, '______s___s_']],
  },
  back: {
    typeA: [[13, '_s__________'], [14, '_c__________']],
    typeB: [[13, '__________s_'], [14, '__________C_']],
  },
};
const ACC = { // 配饰 / 发型也是补丁
  long:    { front: [[8, '.h__________'], [9, '.hh_________'], [10, '.hH_________'], [11, '.hH_________'], [12, '.H__________']],
             back:  [[9, '.hhHHHHHHhh.'], [10, '..hhhhhhhh..'], [11, '..hHhhhhHh..']] },
  tie:     { front: [[12, '______r_____'], [13, '______r_____']] },
  glasses: { front: [[6, '_____g_ggg_g']], back: [[6, '__________g_']] },
  headset: { front: [[1, '__aaaaaaaaa_'], [5, '__aA________'], [6, '__aA________'], [7, '__aA________'], [8, '____a_______'], [9, '____aa______']],
             back:  [[1, '__aaaaaaaaa_'], [5, '_a_________a'], [6, '_a_________a']] },
  cap:     { front: [[0, '....aaaaa...'], [1, '..aaaaaaaaa.'], [2, '.aaaaaaaaaaa'], [3, '________AAAA']],
             back:  [[0, '...aaaaaa...'], [1, '..aaaaaaaaa.'], [2, '.aaaaaaaaaaa'], [3, '.aaaaaaaaaaa']] },

  // ---- 扩充发型（10 种）：只改头发像素，发色跟着 h / H / l 走 ----
  ponytail:  { front: [[2, 'a___________'], [3, 'h___________'], [4, 'h___________'], [5, 'h___________'], [6, 'H___________'], [7, 'h___________'], [8, 'Hh__________'], [9, '.H__________']],
               back:  [[4, '_____aa_____'], [9, '_____hh_____'], [10, '_____hH_____'], [11, '_____hh_____'], [12, '_____H______']] },
  twintails: { front: [[2, 'a___________'], [3, 'h___________'], [4, 'h___________'], [5, 'h___________'], [6, 'H___________'], [7, 'h___________'], [8, 'Hh__________'], [9, '.H_________a'], [10, '___________h'], [11, '__________hH']],
               back:  [[3, 'a__________a'], [4, 'h___________'], [5, 'h___________'], [6, 'H___________'], [7, 'h___________'], [8, 'h__________h'], [9, 'H_________hh'], [10, '_h________hH'], [11, '_H__________']] },
  bun:       { front: [[0, 'hlh.________'], [1, 'hhHa________'], [2, 'H___________']],
               back:  [[1, '____HHHH____'], [2, '___HhllhH___'], [3, '___HhhhhH___'], [4, '____HHHH____'], [5, '_____aa_____']] },
  spiky:     { front: [[0, '...h.h.h.h..'], [1, '.hhhhhhhlhhh'], [2, 'h___________'], [4, 'h___________']],
               back:  [[0, '..h.h.h.h.h.'], [1, '.hhhhhhhhhhh'], [3, 'h___________'], [5, 'h___________']] },
  bald:      { front: [[0, '....sssss...'], [1, '..Sssssssss.'], [2, '.Ssssssswsss'], [3, '.Sssssssssss'], [4, '.SsSssssssss'], [5, '.SsbSsssssss'], [6, '.SsS________'], [7, '.SSs________'], [8, '..Ss________'], [9, '...S________']],
               back:  [[0, '...ssssss...'], [1, '..sssssssss.'], [2, '.sssswssssss'], [3, '.sssssssssss'], [4, '.sssssssssss'], [5, '.sssssssssss'], [6, '.Sssssssssss'], [7, '.SssssssssSs'], [8, '..SSsssssSS.'], [9, '...SSSSSS...']] },
  bangs:     { front: [[4, '.hhhhhhhhhhh'], [5, '_____HhHhHhH']] },
  bob:       { front: [[5, '___________h'], [6, 'hHhh_______h'], [7, 'hHhh_______h'], [8, 'hhhH_______H'], [9, '.HHH________']],
               back:  [[6, 'hhhhhhhhhhhh'], [7, 'hHhhhhhhhhHh'], [8, 'hHHhhhhhhHHh'], [9, '.HHHHHHHHHH.']] },
  afro:      { front: [[0, '..hhlhhhhh..'], [1, '.hhhhhhlhhhh'], [2, 'hhlhhhhhhhhh'], [3, 'hhhhhlhhhhhh'], [4, 'hh_l________'], [5, 'hhlh________'], [6, 'hHhh________'], [7, 'hhHh________'], [8, '.hHh________']],
               back:  [[0, '..hhhlhhhh..'], [1, '.hhlhhhhhlhh'], [2, 'hhhhhhlhhhhh'], [3, 'hlhhhhhhhlhh'], [4, 'hhhhlhhhhhhh'], [5, 'hhhhhhhhlhhh'], [6, 'hhlhhhhhhhhs'], [7, 'hHhhhhlhhhHs'], [8, '.hHHhhhhhHS.']] },
  mohawk:    { front: [[0, '...hhlhh....'], [1, '..HhhhhhHHH.'], [2, '.HHHhhhhHHHH'], [3, '.HHHHhhHHHHH'], [4, '.HHHHHHsHHsH'], [5, '.HHHH_______'], [6, '.HHH________'], [7, '.HHH________'], [8, '..HH________']],
               back:  [[0, '...HHhhHH...'], [1, '..HHHhhHHHH.'], [2, '.HHHHhlHHHHH'], [3, '.HHHHhhHHHHH'], [4, '.HHHHhhHHHHH'], [5, '.HHHHhhHHHHH'], [6, '.HHHHhhHHHHs'], [7, '.HHHHhhHHHHs'], [8, '..HHHhhHHHS.']] },
  braid:     { front: [[8, '_h__________'], [9, '_hH_________'], [10, '__Hh________'], [11, '__hH________'], [12, '__Hh________'], [13, '___a________']],
               back:  [[9, '_____hH_____'], [10, '_____Hh_____'], [11, '_____hH_____'], [12, '_____Hh_____'], [13, '_____aa_____']] },

  // ---- 扩充配饰（10 种）：帽子 / 头饰 / 脸部 / 胸前，颜色走 a / A（accColor）或固定色 ----
  beanie:    { front: [[0, '....aawaa...'], [1, '..aaaaaaaaa.'], [2, '.aaaaaaaaaaa'], [3, '.AAAAAAAAAAA']],
               back:  [[0, '...aawaaa...'], [1, '..aaaaaaaaa.'], [2, '.aaaaaaaaaaa'], [3, '.AAAAAAAAAAA']] },
  headband:  { front: [[3, '.aaaaaaaaaaa']],
               back:  [[3, '.aaaaaaaaaaa'], [4, 'a___________'], [5, 'A___________']] },
  bow:       { front: [[0, 'aa.aa_______'], [1, 'aAAa________'], [2, '.a__________']],
               back:  [[0, '...aa.aa....'], [1, '...aaAAaa...'], [2, '____a__a____']] },
  shades:    { front: [[6, '____gkkgkkkk'], [7, '_____kk__kk_']], back: [[6, '__________g_']] },
  mask:      { front: [[7, '____M_______'], [8, '____Mmmmmmmm'], [9, '____MmmmmmM_']], back: [[7, '__________M_']] },
  beard:     { front: [[7, '____h_______'], [8, '____hhsssshh'], [9, '___HhhhhhhH_']], back: [[8, '__________h_']] },
  mustache:  { front: [[8, '______HhhH__'], [9, '______H__H__']] },
  bowtie:    { front: [[11, '____aaAaa___'], [12, '____a___a___']] },
  badge:     { front: [[11, '____a___a___'], [12, '_____a_a____'], [13, '_____yYy____']], back: [[11, '____aaaa____']] },
  scarf:     { front: [[10, '____aAaAa___'], [11, '___aaaaaaa__'], [12, '____A_______'], [13, '____a_______']],
               back:  [[10, '____aAaAa___'], [11, '___aaaaaa___']] },
};
const luminance = c => { const [r, g, b] = toRGB(c); return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255; };
function lookPal(L) {
  return {
    h: L.hair, H: dark(L.hair, .28), l: lite(L.hair, .35),
    // 深肤色上默认眼色几乎看不见，换成更深的一档
    s: L.skin, S: dark(L.skin, .13), b: mix(L.skin, '#ff6f6f', .35), e: luminance(L.skin) < .45 ? '#140d12' : '#2a2230',
    c: L.shirt, C: dark(L.shirt, .22), w: '#ffffff', p: L.pants || '#3a3f52',
    a: L.accColor || '#454b5e', A: L.accColor ? dark(L.accColor, .25) : '#ff8a5c',
    r: '#d8423a', g: '#9aa3b8',
    k: '#1d2130', m: '#f3f6fb', M: '#c5cdd9', y: '#ffd35c', Y: '#d9a92e',   // 墨镜片 / 口罩 / 工牌卡片
  };
}
// 走路 = 坐姿模板下面再接三行腿脚;手臂沿用打字的两帧补丁当摆臂
const LEGS = {
  stand: ['...pp..pp...', '...pp..pp...', '..kkk..kkk..'],
  walkA: ['..pp....pp..', '.pp......pp.', '.kk......kk.'],
  walkB: ['...pp..pp...', '...pp..pp...', '..kkk..kkk..'],
};
const WALK_ARMS = { walkA: 'typeA', walkB: 'typeB' };
function charSprite(L, pose, frame, flip) {
  return sprite(`ch|${L.key}|${pose}|${frame}|${+flip}`, () => {
    const legs = LEGS[frame];
    let rows = patch(BODY[pose], FRAMES[pose][legs ? WALK_ARMS[frame] : frame]);
    for (const k of L.acc) rows = patch(rows, ACC[k] && ACC[k][pose]);
    if (legs) rows = rows.concat(legs);
    const cv = pix(rows, lookPal(L), flip);
    return { cv, ax: cv.width >> 1, ay: cv.height - 2 };
  });
}

const HAIR_COLORS = ['#231d1c', '#5b3b2b', '#b8683a', '#dcb24c', '#2c2a38', '#8b5a39', '#c94f7c', '#4a6fa5',
  '#b9bcc6', '#f1dca0', '#8e2b3b', '#e0673a', '#5fc8a8', '#9b7fd6', '#5aa9e6', '#f29bb8', '#2f3f7a', '#2c8c8c'];
const SKIN_TONES = ['#f7d5b5', '#eec7a3', '#d9a77f', '#b9825a', '#f9dcc0',
  '#fbe5d6', '#f3cfb3', '#e8b894', '#e0ac7e', '#c68c62', '#a8714a', '#94603e', '#7d4f33', '#6b4430', '#5a3825'];
const SHIRT_COLORS = ['#3aa56b', '#e0574c', '#4a8fdc', '#f2a444', '#7a62d8', '#2fb5b0', '#e36aa6',
  '#f4d35e', '#2e3a59', '#8a9aa9', '#f2f2ec', '#3a3b45', '#a3d977', '#8e3b5f', '#c9975b', '#86c5f0', '#ff8f7a'];
const ACC_COLORS = ['#e8544a', '#3a8cf0', '#45b36a',
  '#f2c14e', '#8e6bd8', '#2fb5b0', '#f08a3c', '#e36aa6', '#3a3f52', '#f4f4f0', '#7a4a2a', '#9fd4ff', '#b8e986'];
// 四个槽位各抽一个，互不打架；空字符串的权重就是「不戴」的概率
const HAIR_STYLES = ['', '', 'long', 'ponytail', 'twintails', 'bun', 'spiky', 'bald', 'bangs', 'bob', 'afro', 'mohawk', 'braid'];
const HEADWEAR = ['', '', '', 'cap', 'beanie', 'headband', 'bow', 'headset'];
const FACEWEAR = ['', '', '', 'glasses', 'shades', 'mask', 'beard', 'mustache'];
const CHESTWEAR = ['', '', 'tie', 'bowtie', 'badge', 'scarf'];
const TALL_HAIR = new Set(['bun', 'spiky', 'afro', 'mohawk']);   // 帽子会把它们压扁
function stableLook(seed) {
  const rand=rng(seed), choose=items=>items[Math.floor(rand()*items.length)];
  const style=choose(HAIR_STYLES);let head=choose(HEADWEAR);
  if(TALL_HAIR.has(style)&&(head==='cap'||head==='beanie'))head='';
  return {hair:choose(HAIR_COLORS),skin:choose(SKIN_TONES),shirt:choose(SHIRT_COLORS),pants:'#3a3f52',acc:[style,head,choose(FACEWEAR),choose(CHESTWEAR)].filter(Boolean),accColor:choose(ACC_COLORS)};
}

function stableSeed(value) {
  let hash = 2166136261;
  for (const c of String(value)) hash = Math.imul(hash ^ c.codePointAt(0), 16777619);
  return hash >>> 0;
}
function characterSprite(look, pose = 'front', frame = 'stand', flip = false) {
  if (look.key) return charSprite(look, pose, frame, flip);
  const normalized = { ...look, acc: look.acc || [] };
  normalized.key = [normalized.hair, normalized.skin, normalized.shirt, normalized.pants,
    normalized.acc.join('+'), normalized.accColor || ''].join('|');
  return charSprite(normalized, pose, frame, flip);
}
root.ACEOfficeCharacters = Object.freeze({stableSeed, stableLook, characterSprite,
  HAIR_COLORS, SKIN_TONES, SHIRT_COLORS, ACC_COLORS, HAIR_STYLES, HEADWEAR,
  FACEWEAR, CHESTWEAR, TALL_HAIR});
})(globalThis);
