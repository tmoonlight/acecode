/* DOM is an adapter: simulation and artwork do not depend on this UI. */
(function (O) {
  'use strict';
  const $=id=>document.getElementById(id);
  const atlas=new O.Atlas(),sim=new O.Simulation(),renderer=new O.Renderer($('office'),atlas,sim);
  const media=window.matchMedia('(prefers-reduced-motion: reduce)'); sim.paused=media.matches;
  const events=new AbortController(),listen=(el,event,handler)=>el.addEventListener(event,handler,{signal:events.signal});
  const rows=[]; let raf=null,lastTime=null,lastUi=0,eventSignature='',toastTimer=null;
  const setText=(el,text)=>{text=String(text);if(el.textContent!==text)el.textContent=text;};
  for(const a of sim.agents) {
    const row=document.createElement('button'); row.type='button';row.className='agent-row';row.dataset.agent=a.index;
    row.innerHTML='<canvas class="avatar" width="35" height="38" aria-hidden="true"></canvas><span><span class="agent-name"></span><span class="agent-role"></span></span><span class="agent-state"></span>';
    row.querySelector('.agent-name').textContent=a.name;row.querySelector('.agent-role').textContent=a.role;
    atlas.portrait(row.querySelector('canvas'),a.index);
    listen(row,'click',()=>select(a.index));$('agent-list').append(row);rows.push(row);
  }
  function select(index) { sim.selected=index;updateUI();renderer.render(); }
  function updateUI() {
    setText($('clock'),sim.clock);setText($('day'),String(1+Math.floor(sim.time/(3*1440))).padStart(2,'0'));
    let working=0;
    for(const a of sim.agents) {
      const row=rows[a.index],status=sim.status(a.index),state=row.querySelector('.agent-state');
      row.setAttribute('aria-pressed',String(sim.selected===a.index));
      row.setAttribute('aria-label',a.name+'，'+a.role+'，'+status.label);
      setText(state,status.label);state.dataset.state=status.state;
      if(a.action==='work')working++;
    }
    setText($('working-count'),working);setText($('resting-count'),(6-working)+' 位待命 / 休息');
    const a=sim.agents[sim.selected],task=sim.tasks[sim.selected],status=sim.status(sim.selected),percent=Math.floor(task.elapsed/task.duration*100);
    setText($('detail-name'),a.name);setText($('detail-role'),a.role);setText($('detail-id'),'AGENT '+String(a.index+1).padStart(2,'0'));
    setText($('detail-state'),status.label);
    const description=task.elapsed>=task.duration?'已完成'+task.title+'。休息一下，准备迎接下一个好想法。':!sim.ready(a.index)?'等待'+task.deps.filter(i=>sim.tasks[i].elapsed<sim.tasks[i].duration).map(i=>sim.agents[i].name).join('、')+'交接，然后开始'+task.title+'。':task.text;
    setText($('detail-task'),description);setText($('detail-percent'),percent+'%');$('detail-progress').style.width=percent+'%';
    setText($('project-title'),sim.title);setText($('project-status'),sim.finished?'作品已完成':sim.paused?'协作已暂停':'团队正在协作');
    setText($('task-count'),sim.doneCount+' / 6 项任务完成');
    const total=Math.floor(sim.progress*100);$('project-percent').firstChild.textContent=total;$('project-fill').style.width=total+'%';
    setText($('progress-caption'),sim.finished?'做得不错！可以派发下一个任务了。':'一步一步，把想法变成作品。');
    setText($('scene-heading'),sim.paused?'把灵感暂停一下':sim.finished?'今天的好想法已完成':'灵感正在发生');
    const dispatch=$('new-project');dispatch.disabled=sim.active;
    dispatch.lastChild.textContent=sim.active?'任务进行中':'派发新任务';dispatch.title=sim.active?'当前项目完成后，可派发下一个演示任务':'开启下一个演示项目';
    $('toggle-pause').setAttribute('aria-pressed',String(sim.paused));
    $('toggle-pause').querySelector('use').setAttribute('href',sim.paused?'#i-play':'#i-pause');
    setText($('toggle-pause').querySelector('span'),sim.paused?'继续':'暂停');$('pause-overlay').hidden=!sim.paused;
    for(const button of document.querySelectorAll('[data-speed]')) button.setAttribute('aria-pressed',String(Number(button.dataset.speed)===sim.speed));
    const signature=sim.events.map(e=>e.time+e.text).join('|');
    if(signature!==eventSignature) {
      eventSignature=signature;$('activity-list').replaceChildren();
      for(const e of sim.events.slice(0,3)) { const li=document.createElement('li'),time=document.createElement('time'),text=document.createElement('span');time.textContent=e.time;text.textContent=e.text;li.append(time,text);$('activity-list').append(li); }
    }
    setText($('zoom-label'),Math.round(renderer.zoom*100)+'%');
    $('zoom-out').disabled=renderer.zoom<=.8;$('zoom-in').disabled=renderer.zoom>=1.4;
  }
  function stop() { if(raf!==null) cancelAnimationFrame(raf);raf=null;lastTime=null; }
  function canAnimate() {return !sim.paused&&!document.hidden&&!$('library').open;}
  function start() { if(raf===null&&canAnimate()) {lastTime=null;raf=requestAnimationFrame(frame);} }
  function frame(now) {
    raf=null;if(!canAnimate()) {lastTime=null;return;}
    if(lastTime===null)lastTime=now;
    const dt=(now-lastTime)/1000;
    if(dt>=1/30) {
      sim.tick(Math.min(dt,.25));lastTime=now;renderer.render();
      if(now-lastUi>220) {updateUI();lastUi=now;}
    }
    raf=requestAnimationFrame(frame);
  }
  function refresh() {updateUI();renderer.render();}
  function togglePause() {sim.paused=!sim.paused;stop();refresh();start();}
  function toast(message) {clearTimeout(toastTimer);setText($('toast'),message);$('toast').hidden=false;toastTimer=setTimeout(()=>{$('toast').hidden=true;},3200);}
  function zoom(delta) {renderer.zoom=Math.round(Math.max(.8,Math.min(1.4,renderer.zoom+delta))*100)/100;refresh();}
  listen($('toggle-pause'),'click',togglePause);
  listen($('new-project'),'click',()=>{if(sim.startProject()) {refresh();toast('新任务已派发，看看大家怎么分工吧。');}});
  for(const button of document.querySelectorAll('[data-speed]')) listen(button,'click',()=>{sim.speed=Number(button.dataset.speed);updateUI();});
  listen($('zoom-in'),'click',()=>zoom(.1));listen($('zoom-out'),'click',()=>zoom(-.1));
  listen($('zoom-reset'),'click',()=>{renderer.zoom=1;refresh();});
  listen($('office'),'pointermove',e=>{const next=renderer.hit(e);$('office').style.cursor=next>=0?'pointer':'default';if(next!==renderer.hover){renderer.hover=next;renderer.render();}});
  listen($('office'),'pointerleave',()=>{renderer.hover=-1;renderer.render();});
  listen($('office'),'click',e=>{const hit=renderer.hit(e);if(hit>=0)select(hit);});
  listen(document,'keydown',e=>{if(e.code==='Space'&&e.target===document.body&&!$('library').open){e.preventDefault();togglePause();}});
  listen(document,'visibilitychange',()=>{stop();start();});
  listen(window,'pagehide',stop);listen(window,'pageshow',start);
  listen(media,'change',e=>{if(e.matches){sim.paused=true;stop();refresh();}});
  const labels=[['tile','共享地砖'],['desk','标准工位'],['chair','办公椅'],['partition','工位隔板'],['bookcase','资料书架'],['cabinet','储物柜'],['plant','绿植'],['server','服务器'],['whiteboard','任务白板'],['sofa','休息沙发'],['table','咖啡小桌'],['coffee','咖啡台']];
  let libraryBuilt=false;
  function buildLibrary() {
    if(libraryBuilt)return;libraryBuilt=true;
    for(const [type,label] of labels) {
      const item=document.createElement('div');item.className='asset-tile';
      const preview=document.createElement('canvas');preview.width=110;preview.height=96;preview.setAttribute('aria-label',label);
      const c=preview.getContext('2d');c.imageSmoothingEnabled=false;
      if(type==='tile')atlas.draw(c,atlas.tile(0),55,27,1.8);
      else {const s=atlas.get(type);c.drawImage(s.canvas,29,51,102,93,0,0,110,96);}
      const caption=document.createElement('span');caption.textContent=label;item.append(preview,caption);$('asset-grid').append(item);
    }
    for(const a of sim.agents) {const item=document.createElement('div'),portrait=document.createElement('canvas');portrait.width=42;portrait.height=50;atlas.portrait(portrait,a.index);const label=document.createElement('span');label.textContent=a.name;item.append(portrait,label);$('portrait-grid').append(item);}
  }
  listen($('open-library'),'click',()=>{buildLibrary();const stats=atlas.stats();setText($('asset-stats'),stats.sprites+' 个缓存图块 · 约 '+stats.kib+' KiB 图块内存 · 0 张外部图片');$('library').showModal();stop();});
  listen($('close-library'),'click',()=>{$('library').close();});
  listen($('library'),'close',start);
  listen($('library'),'click',e=>{if(e.target===$('library')){const r=$('library').getBoundingClientRect();if(e.clientX<r.left||e.clientX>r.right||e.clientY<r.top||e.clientY>r.bottom)$('library').close();}});
  // A small, explicit seam for embedding, debugging and deterministic verification.
  O.demo={simulation:sim,renderer,atlas,refresh,destroy(){stop();events.abort();clearTimeout(toastTimer);}};
  refresh();start();
})(globalThis.PixelOffice = globalThis.PixelOffice || {});
