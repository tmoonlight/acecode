/* Room caching and depth composition use the same atlas as all UI previews. */
(function (O) {
  'use strict';
  const {C,canvas,rect,line,poly,iso,plane,box,panel}=O.Art;
  const origin={x:340,y:155}, size={x:14,y:11};
  const project=(x,y,z=0)=>{ const p=iso(x,y,z); return {x:origin.x+p[0],y:origin.y+p[1]}; };
  function windowPanel(c,start,width,left=false) {
    c.save(); if(left) c.scale(-1,1);
    panel(c,start,-.015,36,width,47,'#aaa995','#808975');
    panel(c,start+.08,.0,39,width-.16,43,'#f8f4e4');
    panel(c,start+.16,.025,42,width-.32,37,'#a4ccda','#8bafba');
    panel(c,start+.18,.03,44,width-.36,13,'#b9dce3');
    for(let i=0;i<5;i++) {
      const x=start+.19+i*(width-.4)/5,h=7+(i*7)%8;
      panel(c,x,.035,43,(width-.4)/5+.01,h,i%2?'#91b9ad':'#a2c5b7');
    }
    for(let i=1;i<3;i++) panel(c,start+width*i/3-.025,.045,42,.075,37,'#f6f3e2','#b1c3bc');
    panel(c,start+.16,.05,61,width-.32,2,'#d3e8e5');
    box(c,start-.04,.04,width+.08,.18,3,35,['#f6f3e7','#cac9b5','#e0dfcc'],'#a5ad97');
    c.restore();
  }
  function tinyTree(c,x,y,scale=1) {
    c.save(); c.translate(x,y); c.scale(scale,scale);
    rect(c,-2,0,4,14,'#91856a');
    for(const [a,b,w,h,color] of [[-10,-13,20,17,'#9bab85'],[-14,-9,27,11,'#94a77b'],[-8,-17,17,11,'#b1bf96'],[-4,-20,10,8,'#b9c59e']]) rect(c,a,b,w,h,color);
    rect(c,-10,-4,6,4,'#82986a'); c.restore();
  }
  function buildRoom(atlas) {
    const result=canvas(768,512),c=result.getContext('2d'); c.imageSmoothingEnabled=false;
    // Garden fragments and the offset foundation ground the cutaway room.
    tinyTree(c,77,303,1.2); tinyTree(c,703,333,.9); tinyTree(c,664,378,.65);
    for(const [x,y] of [[87,332],[663,408],[492,479],[160,393],[58,285]]) {
      rect(c,x,y,2,3,'#b2bfa3'); rect(c,x+4,y-1,1,3,'#a8b798');
    }
    c.save(); c.translate(origin.x,origin.y);
    poly(c,[iso(-.15,.0,-11),iso(14.5,.1,-11),iso(14.5,11.3,-11),iso(-.15,11.3,-11)],'rgba(91,107,73,.16)');
    box(c,0,0,size.x,size.y,9,-9,['#c7d1b9','#a3ac94','#b2b9a2'],'#858f76');
    for(let y=0;y<size.y;y++) for(let x=0;x<size.x;x++) { const p=iso(x,y); atlas.draw(c,atlas.tile((x*7+y*3)%4),p[0],p[1]); }
    // Two walls, with low wooden skirting and a lighter top cap.
    poly(c,[iso(0,0),iso(0,11),iso(0,11,99),iso(0,0,99)],'#e4e0cd','#8c927d');
    poly(c,[iso(0,0),iso(14,0),iso(14,0,99),iso(0,0,99)],'#f0ecdc','#929781');
    poly(c,[iso(0,0,99),iso(14,0,99),iso(14,-.18,103),iso(0,-.18,103),iso(-.18,0,103),iso(-.18,11,103),iso(0,11,99)],'#faf7e9','#afb29c');
    poly(c,[iso(0,0,93),iso(0,11,93),iso(0,11,99),iso(0,0,99)],'#eeeada');
    poly(c,[iso(0,0,92),iso(14,0,92),iso(14,0,99),iso(0,0,99)],'#fff9e7');
    poly(c,[iso(.015,0,0),iso(.015,11,0),iso(.015,11,6),iso(.015,0,6)],'#b7996c','#a18c67');
    panel(c,0,.01,0,14,6,'#c2a375','#a18c67');
    windowPanel(c,1.0,2.6,true); windowPanel(c,4.8,2.6,true);
    windowPanel(c,3.2,2.5); windowPanel(c,7.0,2.5);
    // Warm notice board and a real-sized door occupy the remaining wall bays.
    panel(c,10.25,.02,42,1.65,37,'#9e8055','#7e7557');
    panel(c,10.33,.04,45,1.49,31,'#c8ac77');
    for(const [x,z,color] of [[10.45,59,'#ece7cb'],[11.12,56,'#b3c39a'],[10.60,47,'#e2d29c']]) {
      panel(c,x,.05,z,.48,10,color); const p=iso(x+.23,.06,z+9); rect(c,p[0],p[1],1,2,'#9c7354');
    }
    panel(c,12.5,.02,7,1.17,67,'#aa9473','#85836b');
    panel(c,12.59,.035,8,.98,63,'#c1ab86');
    panel(c,12.68,.05,35,.78,29,'#b8d3cc','#96b2a5');
    const knob=iso(13.42,.06,30); rect(c,knob[0],knob[1],2,3,'#696f59');
    // Pixel clock between the two rear windows.
    const clock=iso(6.34,.04,80);
    rect(c,clock[0]-8,clock[1]-7,16,15,'#81886e'); rect(c,clock[0]-6,clock[1]-9,12,19,'#81886e');
    rect(c,clock[0]-6,clock[1]-6,12,13,'#f6f3dc'); rect(c,clock[0]-4,clock[1]-7,8,15,'#f6f3dc');
    line(c,clock[0],clock[1],clock[0],clock[1]-5,'#535e4d'); line(c,clock[0],clock[1],clock[0]+4,clock[1]+2,'#535e4d');
    const sign=iso(.7,.06,80);
    rect(c,sign[0]-3,sign[1]-11,49,16,'#d9dbc0'); rect(c,sign[0]-2,sign[1]-10,47,14,'#687b56');
    c.fillStyle='#f3efd6'; c.font='bold 7px sans-serif'; c.textAlign='left'; c.fillText('GOOD WORK',sign[0]+1,sign[1]);
    // Sunlight is baked once, below furniture and characters.
    poly(c,[iso(.15,2.0),iso(.15,4.2),iso(3.1,5.4),iso(3.1,3.2)],'rgba(255,247,204,.20)');
    poly(c,[iso(3.5,.12),iso(5.5,.12),iso(7.4,2.1),iso(5.4,2.1)],'rgba(255,250,214,.19)');
    c.restore(); return result;
  }
  class Renderer {
    constructor(target,atlas,simulation) {
      this.canvas=target; this.ctx=target.getContext('2d'); this.atlas=atlas; this.sim=simulation;
      this.zoom=1; this.hover=-1; this.background=buildRoom(atlas); this.hits=[];
      this.furniture=[
        {type:'plant',x:.7,y:1.2}, {type:'bookcase',x:2.0,y:.65},
        {type:'whiteboard',x:.65,y:5.25}, {type:'cabinet',x:.60,y:9.5},
        {type:'coffee',x:10.8,y:.65}, {type:'server',x:13.15,y:1.35},
        {type:'rug',x:2,y:8.9,depth:0}, {type:'sofa',x:1.25,y:7.5}, {type:'table',x:2.1,y:9.35},
        {type:'plant',x:.65,y:10.35}, {type:'planter',x:11.8,y:10.0},
        {type:'plant',x:13.1,y:4.4}, {type:'bookcase',x:13.25,y:6.3},
        {type:'partition',x:4,y:6.05}, {type:'partition',x:7.2,y:6.05}, {type:'partition',x:10.4,y:6.05},
      ];
      for(const a of this.sim.agents) {
        this.furniture.push({type:'desk',x:a.desk.x,y:a.desk.y,variant:a.index===2?'warm':''});
        this.furniture.push({type:'chair',x:a.home.x,y:a.home.y,depth:a.home.x+a.home.y-.1});
      }
      for(const item of this.furniture) item.sprite=atlas.get(item.type,item.variant);
    }
    screen(x,y,z=0) { const p=project(x,y,z); return {x:384+(p.x-384)*this.zoom,y:280+(p.y-280)*this.zoom}; }
    fromPointer(event) {
      const r=this.canvas.getBoundingClientRect(),s=Math.min(r.width/768,r.height/512);
      return {x:(event.clientX-r.left-(r.width-768*s)/2)/s,y:(event.clientY-r.top-(r.height-512*s)/2)/s};
    }
    hit(event) { const p=this.fromPointer(event); for(let i=this.hits.length-1;i>=0;i--) { const h=this.hits[i]; if(p.x>=h.x-17*this.zoom&&p.x<=h.x+18*this.zoom&&p.y>=h.y-52*this.zoom&&p.y<=h.y+7*this.zoom) return h.index; } return -1; }
    bubble(c,x,y,text,color='#526a43') {
      c.font='10px "Microsoft YaHei", sans-serif'; const width=Math.ceil(c.measureText(text).width)+16;
      rect(c,x-width/2,y-19,width,17,'#839375'); rect(c,x-width/2-1,y-17,width+2,13,'#839375');
      rect(c,x-width/2+1,y-18,width-2,15,'#fffdf1'); rect(c,x-width/2,y-16,width,11,'#fffdf1');
      poly(c,[[x-3,y-2],[x+3,y-2],[x,y+2]],'#839375'); poly(c,[[x-2,y-3],[x+2,y-3],[x,y]],'#fffdf1');
      c.fillStyle=color;c.textAlign='center';c.fillText(text,Math.round(x),Math.round(y-7));
    }
    render() {
      const c=this.ctx,sim=this.sim,atlas=this.atlas;
      c.imageSmoothingEnabled=false; c.clearRect(0,0,768,512); c.save();
      c.translate(384,280);c.scale(this.zoom,this.zoom);c.translate(-384,-280);
      c.drawImage(this.background,0,0);
      const selected=sim.agents[sim.selected],sp=project(selected.x,selected.y);
      c.save();c.translate(sp.x,sp.y+1);plane(c,-.47,-.43,.94,.86,0,'rgba(244,244,197,.4)','#829c61'); c.restore();
      const items=this.furniture.map(item=>({...item,depth:item.depth===undefined?item.x+item.y:item.depth}));
      for(const a of sim.agents) items.push({type:'person',x:a.x,y:a.y,agent:a,depth:a.x+a.y+.05});
      items.sort((a,b)=>a.depth-b.depth);
      this.hits=[];
      for(const item of items) {
        const p=project(item.x,item.y);
        if(item.type==='person') {
          const a=item.agent,frame=a.action==='walk'?Math.floor(sim.time*7+a.index)%4:a.action==='idle'?(Math.floor(sim.time+a.index*1.7)%6===0?1:0):Math.floor(sim.time*4+a.index)%2;
          atlas.draw(c,atlas.person(a.index,a.action,frame),p.x,p.y);
          this.hits.push({...this.screen(a.x,a.y),index:a.index});
        } else atlas.draw(c,item.sprite,p.x,p.y);
      }
      // Name plates are a display layer; all hit tests target real actor positions.
      for(const a of sim.agents) {
        const p=project(a.x,a.y),active=a.index===sim.selected||a.index===this.hover;
        c.font='8px "Microsoft YaHei", sans-serif'; c.textAlign='center';
        const name=a.name,width=24;
        rect(c,p.x-width/2,p.y+5,width,12,active?'#506947':'rgba(249,249,232,.90)');
        c.fillStyle=active?'#fffce8':'#5c7050'; c.fillText(name,p.x,p.y+14);
      }
      for(const a of sim.agents) {
        const selected=a.index===sim.selected;
        const show=selected||(a.action==='work'&&Math.floor(sim.time/4+a.index)%3===0)||(a.action==='coffee');
        if(!show)continue;
        const p=project(a.x,a.y),status=sim.status(a.index);
        const text=a.action==='coffee'?'补充一点灵感':status.label+(a.action==='work'?'...':'');
        this.bubble(c,p.x+4,p.y-53,text,selected?'#46653c':'#677a58');
      }
      // Shared coffee corner: tiny steam frames animate without rebuilding furniture.
      const coffee=project(10.55,.95,50),phase=Math.floor(sim.time*3)%3;
      line(c,coffee.x,coffee.y-3-phase,coffee.x+1,coffee.y-7-phase,'#ecebd6');
      if(sim.finished) { const p=project(7,4,82); this.bubble(c,p.x,p.y,'好想法，完成了！'); }
      c.restore();
    }
  }
  O.Renderer=Renderer;
})(globalThis.PixelOffice = globalThis.PixelOffice || {});
