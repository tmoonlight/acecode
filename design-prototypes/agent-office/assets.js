/* Shared, code-drawn pixel atlas. No image files, fonts, network or libraries. */
(function (O) {
  'use strict';
  const C = {
    ink: '#41483f', edge: '#69715e', cream: '#f6f1df', wall: '#e8e5d4',
    blue: '#548cb4', blueLight: '#75aed1', blueDark: '#3e6688',
    wood: '#b78a56', woodLight: '#d2ad72', woodDark: '#8e6b49',
    green: '#658c48', greenLight: '#91b75e', greenDark: '#3d6339',
  };
  function canvas(w, h) { const c = document.createElement('canvas'); c.width = w; c.height = h; return c; }
  function rect(c, x, y, w, h, color) {
    c.fillStyle = color; c.fillRect(Math.round(x), Math.round(y), Math.round(w), Math.round(h));
  }
  function line(c, x0, y0, x1, y1, color) {
    x0 = Math.round(x0); y0 = Math.round(y0); x1 = Math.round(x1); y1 = Math.round(y1);
    const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    c.fillStyle = color;
    for (;;) {
      c.fillRect(x0, y0, 1, 1);
      if (x0 === x1 && y0 === y1) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }
  function poly(c, points, fill, stroke) {
    const p = points.map(([x, y]) => [Math.round(x), Math.round(y)]);
    const lo = Math.min(...p.map(v => v[1])), hi = Math.max(...p.map(v => v[1]));
    c.fillStyle = fill;
    for (let y = lo; y <= hi; y++) {
      const crosses = [];
      for (let i = 0; i < p.length; i++) {
        const a = p[i], b = p[(i + 1) % p.length];
        if ((a[1] <= y && b[1] > y) || (b[1] <= y && a[1] > y)) {
          crosses.push(a[0] + (y - a[1]) * (b[0] - a[0]) / (b[1] - a[1]));
        }
      }
      crosses.sort((a, b) => a - b);
      for (let i = 0; i + 1 < crosses.length; i += 2) {
        c.fillRect(Math.ceil(crosses[i]), y, Math.floor(crosses[i + 1]) - Math.ceil(crosses[i]) + 1, 1);
      }
    }
    if (stroke) p.forEach((v, i) => line(c, ...v, ...p[(i + 1) % p.length], stroke));
  }
  const iso = (x, y, z = 0) => [Math.round((x - y) * 24), Math.round((x + y) * 12 - z)];
  function plane(c, x, y, w, d, z, color, stroke) {
    poly(c, [iso(x, y, z), iso(x + w, y, z), iso(x + w, y + d, z), iso(x, y + d, z)], color, stroke);
  }
  function box(c, x, y, w, d, h, z, colors, stroke = C.ink) {
    const a = iso(x, y, z), b = iso(x + w, y, z), e = iso(x + w, y + d, z), f = iso(x, y + d, z);
    const at = iso(x, y, z + h), bt = iso(x + w, y, z + h), et = iso(x + w, y + d, z + h), ft = iso(x, y + d, z + h);
    poly(c, [bt, et, e, b], colors[2], stroke);
    poly(c, [ft, et, e, f], colors[1], stroke);
    poly(c, [at, bt, et, ft], colors[0], stroke);
  }
  function panel(c, x, y, z, w, h, fill, stroke) {
    poly(c, [iso(x, y, z), iso(x + w, y, z), iso(x + w, y, z + h), iso(x, y, z + h)], fill, stroke);
  }
  function shadow(c, w, d) { plane(c, -w / 2 + .13, -d / 2 + .16, w, d, 0, 'rgba(64,82,56,.16)'); }
  const metal = ['#eff0e4', '#c1c5b7', '#d6d9cc'];
  const wood = [C.woodLight, C.woodDark, C.wood];
  function mug(c, x, y, z, color = '#f4e9cd') {
    box(c, x, y, .14, .14, 5, z, [color, '#c6b693', color]);
    const p = iso(x + .14, y + .03, z + 3);
    rect(c, p[0], p[1], 3, 3, C.ink); rect(c, p[0], p[1], 2, 1, color);
  }
  function desk(c, variant) {
    shadow(c, 1.8, 1.3);
    for (const [x, y] of [[-.74,-.42],[.58,-.42],[-.74,.37],[.58,.37]]) {
      box(c, x, y, .13, .13, 23, 0, metal);
    }
    box(c, .38, -.37, .32, .74, 15, 6, metal);
    for (const z of [10,15,20]) panel(c, .42, .38, z, .25, 1, '#8d9686');
    box(c, -.83, -.51, 1.66, 1.10, 4, 23, metal);
    plane(c, -.74, -.44, 1.50, .94, 28, '#f9f8f0');
    box(c, -.16, -.32, .32, .25, 2, 28, ['#7a837a','#5c635c','#68716b']);
    box(c, -.03, -.27, .09, .09, 6, 29, metal);
    box(c, -.36, -.36, .79, .12, 19, 34, ['#546369','#353f44','#4a5559']);
    panel(c, -.30, -.231, 36, .66, 15, variant === 'warm' ? '#bb976a' : '#83b8c7');
    panel(c, -.25, -.226, 38, .56, 10, variant === 'warm' ? '#e4be85' : '#b3d9d5');
    for (let i = 0; i < 4; i++) panel(c, -.20, -.22, 39 + i * 2, .15 + (i % 3) * .09, 1, i % 2 ? '#579393' : '#eef4d7');
    const p = iso(.30, -.22, 35); rect(c, p[0], p[1], 1, 1, '#b9d786');
    box(c, -.21, .13, .69, .24, 1, 28, ['#9ba8a2','#88958e','#88958e']);
    for (let i = 0; i < 5; i++) plane(c, -.17 + i * .12, .16, .06, .13, 30, '#e1e7d9');
    plane(c, .60, .10, .18, .28, 28, '#b4c4b1');
    box(c, .64, .16, .08, .12, 2, 28, metal);
    mug(c, -.65, .18, 28, variant === 'warm' ? '#b48256' : '#98ad7d');
  }
  function chair(c) {
    shadow(c, .70, .65);
    const foot = iso(0, 0, 3);
    for (const [x, y] of [[-10,4],[10,4],[0,-5]]) {
      line(c, ...foot, x, y, '#535c53'); rect(c, x - 1, y, 3, 3, '#434a43');
    }
    box(c, -.06, -.05, .12, .12, 11, 3, ['#b4bbae','#7d857a','#8d9589']);
    box(c, -.27, -.27, .55, .54, 4, 13, [C.blueLight,C.blueDark,C.blue]);
    box(c, -.30, .25, .60, .11, 16, 18, [C.blueLight,C.blueDark,C.blue]);
    line(c, -11, -17, -11, -10, '#606a60'); line(c, -11, -17, -5, -14, '#414c46');
  }
  function plant(c, small) {
    shadow(c, .48, .48);
    box(c, -.18, -.18, .36, .36, small ? 8 : 13, 0, ['#eae9d9','#b4b6a5','#d2d4c1']);
    plane(c, -.13, -.13, .26, .26, small ? 9 : 14, '#6f6647');
    const s = small ? .6 : 1;
    c.save(); c.scale(s, s);
    line(c, 0, -12, 0, -47, '#5f743a'); line(c, 1, -17, 1, -43, '#8a9c49');
    for (const [x,y,flip] of [[-2,-25,1],[2,-34,-1],[-2,-40,1],[2,-46,-1],[-1,-33,1],[1,-25,-1]]) {
      const p = [[x,y],[x+flip*12,y-12],[x+flip*9,y-16],[x+flip*3,y-12],[x,y-2]];
      poly(c,p,C.greenDark); poly(c,p.map(([a,b])=>[a+flip,b-1]),C.green);
      line(c,x,y-2,x+flip*8,y-12,C.greenLight);
    }
    poly(c,[[0,-40],[-4,-55],[0,-59],[4,-53],[2,-40]],C.greenDark);
    line(c,0,-43,0,-56,C.greenLight); c.restore();
  }
  function bookcase(c) {
    shadow(c, 1.18, .65);
    box(c, -.58, -.28, 1.16, .57, 63, 0, ['#edeada','#b5b7a5','#c5c9b7']);
    panel(c, -.50, .30, 5, 1.0, 52, '#888d7d', C.ink);
    const books = ['#d0af63','#74969f','#a2654f','#718668','#e1d9b5','#667f9f'];
    for (let row = 0; row < 3; row++) {
      for (let i = 0; i < 6; i++) {
        const z = 7 + row * 17, height = 11 + (i * 3 + row) % 5;
        panel(c, -.45 + i * .152, .33, z, .12, height, books[(i + row * 2) % books.length], '#696c58');
        panel(c, -.43 + i * .152, .335, z + 3, .07, 1, '#e4dfc9');
      }
      box(c, -.51, .18, 1.02, .20, 2, 5 + row * 17, metal);
    }
  }
  function cabinet(c) {
    shadow(c, 1.05, .64); box(c, -.52, -.32, 1.04, .64, 59, 0, metal);
    panel(c, -.44, .33, 5, .87, 48, '#d0d3c4', '#858d7c');
    const a = iso(0,.34,6), b=iso(0,.34,52); line(c,...a,...b,'#868d7d');
    for(const x of [-.09,.09]) { const p=iso(x,.35,29); rect(c,p[0],p[1],1,7,'#6e786b'); }
  }
  function server(c) {
    shadow(c, .9, .75); box(c,-.45,-.34,.9,.68,62,0,['#737f79','#45524e','#566560']);
    for (let i=0;i<5;i++) {
      panel(c,-.35,.35,5+i*11,.70,8,'#2f3f39','#879187');
      panel(c,-.27,.36,8+i*11,.30,1,'#64766b');
      const p=iso(.23,.36,10+i*11); rect(c,p[0],p[1],2,2,i===3?'#d6b468':'#a1bd74');
    }
  }
  function whiteboard(c) {
    for(const x of [-.75,.75]) { box(c,x,-.10,.09,.13,50,0,metal); box(c,x-.17,-.12,.42,.27,2,0,metal); }
    box(c,-.92,-.14,1.85,.15,37,18,metal); panel(c,-.83,.02,22,1.67,30,'#f8f8ec','#a7af9b');
    const colors=['#c49b63','#92aa76','#779eab','#c19b8b'];
    for(let row=0;row<2;row++) for(let i=0;i<4;i++) panel(c,-.67+i*.36,.035,27+row*12,.23,7,colors[(i+row)%4]);
    panel(c,-.65,.04,46,1.13,1,'#829172'); box(c,-.91,.02,1.87,.13,2,18,metal);
  }
  function sofa(c) {
    shadow(c,1.90,.90);
    for(const x of [-.8,.65]) box(c,x,-.20,.13,.13,7,0,wood);
    box(c,-.94,-.32,1.88,.78,12,5,['#c99368','#966843','#ad7953']);
    box(c,-.94,-.37,1.88,.20,25,12,['#dcaf80','#b4855e','#c39166']);
    for(const x of [-.91,.68]) box(c,x,-.22,.24,.80,14,13,['#dca374','#ad7a52','#c59263']);
    for(const x of [-.62,.06]) box(c,x,-.09,.61,.52,3,19,['#deb487','#bd946c','#c99c71']);
    box(c,-.5,-.03,.31,.19,9,22,['#b2bf91','#88956e','#9eaa80']);
  }
  function table(c) {
    shadow(c,1.15,.70);
    for(const x of [-.43,.35]) for(const y of [-.19,.22]) box(c,x,y,.08,.08,13,0,wood);
    box(c,-.60,-.34,1.2,.75,3,13,wood);
    plane(c,-.34,-.20,.40,.34,17,'#e7e1c8','#87907a');
    plane(c,-.31,-.17,.32,.25,18,'#9caf93'); mug(c,.21,.0,17);
  }
  function coffee(c) {
    shadow(c,1.90,.78); box(c,-.95,-.36,1.90,.72,27,0,wood);
    for(const x of [-.85,-.26,.33]) { panel(c,x,.37,3,.51,20,'#cba276','#846747'); const p=iso(x+.18,.38,20); rect(c,p[0],p[1],4,1,'#6d6248'); }
    box(c,-1,-.39,2,.82,3,27,['#eee9d7','#c1bca9','#d9d5bf']);
    box(c,-.67,-.20,.55,.45,18,30,['#888c7d','#4b5850','#616a60']);
    panel(c,-.59,.26,35,.38,10,'#293d36'); panel(c,-.54,.27,42,.28,2,'#b8c7a5');
    mug(c,-.46,.21,32); mug(c,.13,.10,30); mug(c,.43,.10,30,'#a4b087');
    box(c,.68,-.17,.16,.16,7,30,['#d1ab73','#b08452','#c69c64']);
  }
  function partition(c) {
    box(c,-.85,-.09,1.7,.18,31,0,['#acbf82','#8ea565','#9eb578']);
    for(const x of [-.85,.8]) box(c,x,-.12,.06,.24,34,0,metal);
    box(c,-.90,-.12,1.8,.23,2,34,metal);
  }
  function planter(c) {
    box(c,-1.13,-.24,2.26,.48,15,0,['#edeedd','#bbbdaa','#d1d4bf']);
    plane(c,-1.05,-.17,2.1,.34,16,'#77794e');
    for(let i=0;i<9;i++) {
      const p=iso(-.97+i*.23,0,19); rect(c,p[0]-3,p[1]-9,6,12,i%2?'#668349':'#7e9958');
      rect(c,p[0]-1,p[1]-12,3,8,'#a0b976');
    }
  }
  function rug(c) { plane(c,-1.2,-.9,2.4,1.8,.2,'#d2bea0','#a49679'); plane(c,-1.05,-.74,2.1,1.48,.4,'#c7b797','#b8a88a'); }
  const furniture = { desk, chair, plant, bookcase, cabinet, server, whiteboard, sofa, table, coffee, partition, planter, rug };
  const profiles = [
    { id:'ce', name:'阿策', role:'团队协调', color:'#b28c57', dark:'#7e6244', hair:'#554936', lightHair:'#7a6748', skin:'#edc395', style:0 },
    { id:'ma', name:'小码', role:'全栈开发', color:'#628ea8', dark:'#436378', hair:'#343e3c', lightHair:'#535c50', skin:'#e8bd8f', style:1 },
    { id:'hui', name:'阿绘', role:'视觉设计', color:'#c48568', dark:'#945e48', hair:'#78543f', lightHair:'#9b7351', skin:'#f2cea2', style:2 },
    { id:'xun', name:'小寻', role:'资料研究', color:'#a497b7', dark:'#756c87', hair:'#504637', lightHair:'#70624a', skin:'#e7bb87', style:3 },
    { id:'shen', name:'阿审', role:'代码审查', color:'#84946b', dark:'#5a6d49', hair:'#4e493e', lightHair:'#77705c', skin:'#dba977', style:1 },
    { id:'ce2', name:'小测', role:'质量测试', color:'#c1a157', dark:'#8a733e', hair:'#654c36', lightHair:'#96744c', skin:'#f1c89b', style:0 },
  ];
  // All six people share this silhouette. Palette/style/frame are the only variants.
  function person(c, p, action, frame) {
    const seated = action === 'work', walking = action === 'walk';
    const bob = walking ? frame % 2 : 0;
    const oy = seated ? -5 : -bob;
    c.save(); c.translate(0,oy);
    if (!seated) { rect(c,-8,0,18,3,'rgba(52,65,43,.16)'); }
    const leg = walking ? [0,2,0,-2][frame%4] : 0;
    rect(c,-6,-12,6,11+leg,'#374640'); rect(c,2,-12,6,11-leg,'#43534a');
    rect(c,-7,-3+leg,8,4,C.ink); rect(c,2,-3-leg,9,4,C.ink);
    if(seated) { rect(c,-6,-5,6,6,'#b7b3a0'); rect(c,1,-5,6,6,'#c6c0a6'); }
    poly(c,[[-8,-26],[5,-27],[10,-23],[9,-11],[-7,-10],[-10,-20]],C.ink);
    rect(c,-7,-25,13,14,p.color); rect(c,-7,-22,4,10,p.dark); rect(c,-2,-25,6,3,'#eee9d7');
    rect(c,2,-23,3,10,p.color); rect(c,6,-21,3,7,p.dark);
    if(action==='work') {
      const k=frame%2; rect(c,6,-19-k,6,4,p.color); rect(c,10,-21-k,5,4,p.skin);
      rect(c,-2,-16+k,9,3,p.color); rect(c,6,-18+k,5,3,p.skin);
    } else if(action==='coffee') {
      rect(c,6,-22,4,8,p.color); rect(c,8,-25,4,4,p.skin);
      rect(c,10,-28,6,7,C.ink); rect(c,11,-27,5,5,'#e8e1c8'); rect(c,15,-26,3,3,'#cfc7ad');
    } else {
      rect(c,-10,-22,3,10,p.dark); rect(c,-10,-13+(walking?leg:0),3,4,p.skin);
      rect(c,8,-21,3,9,p.color); rect(c,8,-12-(walking?leg:0),3,4,p.skin);
    }
    // Large expressive head, three-quarter view toward the desk.
    poly(c,[[-9,-40],[-6,-44],[5,-44],[10,-39],[10,-29],[6,-25],[-5,-25],[-10,-30]],C.ink);
    rect(c,-7,-39,15,11,p.skin); rect(c,-5,-29,11,3,p.skin);
    rect(c,-8,-35,3,6,p.skin); rect(c,8,-34,3,4,p.skin);
    rect(c,-7,-41,13,4,p.hair); rect(c,-9,-38,6,7,p.hair); rect(c,-7,-43,12,3,p.hair);
    rect(c,-5,-42,8,2,p.lightHair); rect(c,-8,-37,3,9,p.hair);
    rect(c,-4,-39,4,2,p.hair); rect(c,3,-40,5,3,p.hair);
    if(p.style===1) { rect(c,-9,-40,5,3,p.hair); rect(c,-5,-45,8,3,p.hair); }
    if(p.style===2) { rect(c,-12,-36,4,14,p.hair); rect(c,-11,-36,2,9,p.lightHair); rect(c,7,-39,3,6,p.hair); rect(c,-10,-29,3,2,p.color); }
    if(p.style===3) { rect(c,-9,-44,16,4,p.dark); rect(c,-11,-40,22,3,p.color); rect(c,-4,-45,9,1,p.color); }
    const blink = action==='idle' && frame===1;
    rect(c,1,-35,2,blink?1:3,'#3e4136'); rect(c,6,-34,2,blink?1:3,'#3e4136');
    rect(c,5,-28,3,1,'#b77959'); rect(c,8,-30,2,1,'#d8a278');
    if(p.id==='shen') { line(c,-1,-36,4,-34,'#424e40'); line(c,5,-34,9,-33,'#424e40'); rect(c,-1,-35,5,4,'#414a3e'); rect(c,0,-34,3,2,'#b9c8af'); rect(c,6,-33,3,2,'#b9c8af'); }
    c.restore();
  }
  class Atlas {
    constructor() { this.cache = new Map(); }
    get(type, variant='') {
      const key=type+':'+variant;
      if(this.cache.has(key)) return this.cache.get(key);
      const bitmap=canvas(160,160), c=bitmap.getContext('2d'); c.imageSmoothingEnabled=false;
      c.translate(80,132);
      if(!furniture[type]) throw new Error('Unknown office asset: '+type);
      furniture[type](c,variant);
      const sprite={canvas:bitmap,ax:80,ay:132}; this.cache.set(key,sprite); return sprite;
    }
    person(index,action='idle',frame=0) {
      const count=action==='walk'?4:2; frame=((frame%count)+count)%count;
      const key='person:'+index+':'+action+':'+frame;
      if(this.cache.has(key)) return this.cache.get(key);
      const bitmap=canvas(48,64),c=bitmap.getContext('2d'); c.translate(22,54);
      person(c,profiles[index],action,frame);
      const sprite={canvas:bitmap,ax:22,ay:54}; this.cache.set(key,sprite); return sprite;
    }
    tile(index=0) {
      const key='tile:'+index; if(this.cache.has(key)) return this.cache.get(key);
      const bitmap=canvas(49,25),c=bitmap.getContext('2d');
      const colors=['#c1cdb3','#c4cfb8','#bfccb2','#c7d1bb'];
      poly(c,[[24,0],[48,12],[24,24],[0,12]],colors[index%4]);
      line(c,0,12,24,0,'#d6dfcc'); line(c,24,0,48,12,'#d3ddc7');
      line(c,0,12,24,24,'#aebfa2'); line(c,24,24,48,12,'#b0bfa4');
      rect(c,15+(index%2)*8,13,1,1,'#b7c7a8');
      const sprite={canvas:bitmap,ax:24,ay:0}; this.cache.set(key,sprite); return sprite;
    }
    draw(c,sprite,x,y,scale=1) { c.drawImage(sprite.canvas,Math.round(x-sprite.ax*scale),Math.round(y-sprite.ay*scale),sprite.canvas.width*scale,sprite.canvas.height*scale); }
    portrait(target,index) { const c=target.getContext('2d'); c.imageSmoothingEnabled=false; c.clearRect(0,0,target.width,target.height); const s=this.person(index); c.drawImage(s.canvas,7,7,30,36,0,0,target.width,target.height); }
    stats() { let bytes=0; for(const s of this.cache.values()) bytes+=s.canvas.width*s.canvas.height*4; return {sprites:this.cache.size,kib:Math.round(bytes/1024)}; }
  }
  O.Art={C,canvas,rect,line,poly,iso,plane,box,panel}; O.Atlas=Atlas; O.profiles=profiles;
})(globalThis.PixelOffice = globalThis.PixelOffice || {});
