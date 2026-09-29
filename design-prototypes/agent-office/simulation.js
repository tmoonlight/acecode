/* Pure, deterministic simulation: explicit time in, structured team state out. */
(function (O) {
  'use strict';
  const desks = [[4,3.5],[7.2,3.5],[10.4,3.5],[4,6.7],[7.2,6.7],[10.4,6.7]];
  const definitions = [
    { title:'拆解需求', doing:'规划中', text:'把好想法拆成可执行的小任务，安排大家的分工。', duration:18, deps:[] },
    { title:'开发功能', doing:'开发中', text:'搭建工作台，编写交互逻辑，让每个小功能运转起来。', duration:48, deps:[0] },
    { title:'设计界面', doing:'设计中', text:'打磨布局、颜色与像素细节，给好想法一个好看的样子。', duration:35, deps:[0] },
    { title:'资料研究', doing:'研究中', text:'收集参考，梳理技术方案，把发现分享给伙伴。', duration:30, deps:[] },
    { title:'代码审查', doing:'审查中', text:'和开发伙伴一起检查实现，找出可以做得更好的地方。', duration:23, deps:[1,2,3] },
    { title:'质量验证', doing:'测试中', text:'验证关键交互和边界情况，确保作品可以放心交付。', duration:22, deps:[4] },
  ];
  const titles=['做一个有趣的 AI 工作台','为小团队设计一个灵感收集器','做一个会照顾番茄钟的助手','把今天的好想法变成小网站'];
  const distance=(a,b)=>Math.hypot(a.x-b.x,a.y-b.y);
  class Simulation {
    constructor() {
      this.time=0; this.speed=1; this.paused=false; this.projectNumber=0; this.selected=1; this.events=[];
      this.agents=O.profiles.map((p,i)=>({ ...p,index:i,desk:{x:desks[i][0],y:desks[i][1]},home:{x:desks[i][0],y:desks[i][1]+.96},x:desks[i][0],y:desks[i][1]+.96,action:'idle',route:[],destination:'home',nextBreak:8+i*3,coffeeUntil:0 }));
      this.startProject(true);
    }
    get active() { return this.tasks.some(t=>t.elapsed<t.duration); }
    get progress() { return this.tasks.reduce((n,t)=>n+t.elapsed/t.duration,0)/this.tasks.length; }
    get doneCount() { return this.tasks.filter(t=>t.elapsed>=t.duration).length; }
    get clock() { const m=9*60+41+Math.floor(this.time/3); return String(Math.floor(m/60)%24).padStart(2,'0')+':'+String(m%60).padStart(2,'0'); }
    log(text) { this.events.unshift({time:this.clock,text}); if(this.events.length>5) this.events.length=5; }
    startProject(initial=false) {
      if(!initial&&this.active) return false;
      this.title=titles[this.projectNumber%titles.length]; this.projectNumber++;
      this.tasks=definitions.map(d=>({...d,deps:[...d.deps],elapsed:0}));
      if(initial) { this.tasks[0].elapsed=18; this.tasks[1].elapsed=8; this.tasks[2].elapsed=9; this.tasks[3].elapsed=19; }
      this.finished=false;
      this.log(initial?'阿策已拆解需求，伙伴们开始协作':'新任务已派发，阿策开始拆解需求');
      if(initial) { this.log('小寻整理了第一批参考资料'); this.log('阿绘和小码开始并行设计、开发'); }
      this.refreshActions(); return true;
    }
    ready(i) { const t=this.tasks[i]; return t.elapsed<t.duration&&t.deps.every(d=>this.tasks[d].elapsed>=this.tasks[d].duration); }
    atHome(a) { return distance(a,a.home)<.02; }
    coffeeRoute(a) {
      const lane=a.desk.y+1.9;
      return [{...a.home},{x:a.home.x,y:lane},{x:12.15,y:lane},{x:12.15,y:1.9},{x:10.8,y:1.9}];
    }
    goCoffee(a) { a.destination='coffee'; a.route=this.coffeeRoute(a).slice(1); a.action='walk'; }
    goHome(a) { a.destination='home'; a.route=this.coffeeRoute(a).slice(0,-1).reverse(); a.action='walk'; }
    move(a,dt) {
      let remaining=dt*1.5;
      while(a.route.length&&remaining>0) {
        const target=a.route[0],d=distance(a,target);
        if(d<=remaining) { a.x=target.x; a.y=target.y; remaining-=d; a.route.shift(); }
        else { a.x+=(target.x-a.x)/d*remaining; a.y+=(target.y-a.y)/d*remaining; remaining=0; }
      }
      if(!a.route.length) {
        if(a.destination==='coffee') { a.action='coffee'; a.coffeeUntil=this.time+5; }
        else { a.action='idle'; a.nextBreak=this.time+14+a.index*2; }
      }
    }
    refreshActions() {
      for(const a of this.agents) if(!a.route.length&&this.atHome(a)) a.action=this.ready(a.index)?'work':'idle';
    }
    tick(dt) {
      if(this.paused||!Number.isFinite(dt)||dt<=0) return;
      // Substeps preserve dependency ordering even for a long caller-supplied step.
      let remaining=dt*this.speed;
      while(remaining>0) { const step=Math.min(.1,remaining); this.step(step); remaining-=step; }
    }
    step(dt) {
      this.time+=dt;
      for(const a of this.agents) {
        const t=this.tasks[a.index];
        if(a.route.length) { this.move(a,dt); continue; }
        if(a.action==='coffee') {
          if(this.ready(a.index)||this.time>=a.coffeeUntil) this.goHome(a);
          continue;
        }
        if(this.atHome(a)&&this.ready(a.index)) {
          a.action='work'; const before=t.elapsed; t.elapsed=Math.min(t.duration,t.elapsed+dt);
          if(before<t.duration&&t.elapsed>=t.duration) { this.log(a.name+'完成了'+t.title); a.action='idle'; a.nextBreak=this.time+2+a.index; }
        } else {
          a.action='idle';
          const coffeeBusy=this.agents.some(other=>other!==a&&other.destination==='coffee'&&(other.route.length||other.action==='coffee'));
          if(this.time>=a.nextBreak&&!coffeeBusy) this.goCoffee(a);
        }
      }
      if(!this.active&&!this.finished) { this.finished=true; this.log('作品完成！大家一起把想法变成了现实'); }
    }
    status(index) {
      const a=this.agents[index],t=this.tasks[index];
      if(a.action==='walk') return {label:a.destination==='home'?'回工位':'走动中',state:'walking'};
      if(a.action==='coffee') return {label:'咖啡时间',state:'coffee'};
      if(a.action==='work') return {label:t.doing,state:'working'};
      return {label:t.elapsed>=t.duration?'已完成':'准备中',state:t.elapsed>=t.duration?'done':'waiting'};
    }
  }
  O.Simulation=Simulation; O.desks=desks;
})(globalThis.PixelOffice = globalThis.PixelOffice || {});
