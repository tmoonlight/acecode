#include "browser_tools.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace acecode::agent_browser {
namespace {

const char* pointer_renderer_source() {
    return R"JS((payload) => {
 const root=document.documentElement;
 const x=Number(payload?.x);
 const y=Number(payload?.y);
 const action=['click','scroll','drag'].includes(payload?.action)?payload.action:'hover';
 if(!root||!Number.isFinite(x)||!Number.isFinite(y))return {ok:false};
 const id='__acecode_agent_browser_ai_pointer_v1';
 let host=document.getElementById(id);
 if(host&&(!host.shadowRoot||!host.shadowRoot.querySelector('.ace-pointer-wrap'))){host.remove();host=null;}
 if(!host){
  host=document.createElement('div');
  host.id=id;
  host.setAttribute('aria-hidden','true');
  host.setAttribute('role','presentation');
  const shadow=host.attachShadow({mode:'open'});
  const style=document.createElement('style');
  style.textContent=`
   :host{all:initial}
   *{box-sizing:border-box;pointer-events:none!important}
   .ace-pointer-wrap{position:absolute;left:0;top:0;width:0;height:0;pointer-events:none;isolation:isolate}
   .ace-pointer-cursor{position:absolute;left:-2px;top:-2px;width:24px;height:30px;overflow:visible;filter:drop-shadow(0 2px 4px rgba(15,23,42,.38))}
   .ace-pointer-badge{position:absolute;left:15px;top:16px;min-width:22px;height:16px;padding:0 5px;border:1px solid rgba(255,255,255,.92);border-radius:8px;background:linear-gradient(135deg,#2563eb,#7c3aed);box-shadow:0 2px 7px rgba(30,41,59,.34);color:#fff;font:700 10px/14px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;letter-spacing:.3px;text-align:center;white-space:nowrap}
   .ace-pointer-ring{position:absolute;left:-16px;top:-16px;width:32px;height:32px;border:2px solid rgba(99,102,241,.9);border-radius:50%;background:rgba(96,165,250,.12);box-shadow:0 0 0 1px rgba(255,255,255,.9),0 0 14px rgba(99,102,241,.38);opacity:.62;transform:scale(.72)}
   .ace-pointer-wrap[data-action="click"] .ace-pointer-ring{animation:ace-pointer-click 680ms cubic-bezier(.16,1,.3,1) 1}
   .ace-pointer-wrap[data-action="scroll"] .ace-pointer-ring{border-style:dashed;animation:ace-pointer-scroll 760ms ease-out 1}
   .ace-pointer-wrap[data-action="hover"] .ace-pointer-ring{opacity:.72;transform:scale(.62);box-shadow:0 0 0 1px rgba(255,255,255,.9),0 0 18px rgba(37,99,235,.45)}
   .ace-pointer-wrap[data-action="drag"] .ace-pointer-ring{border-style:dashed;opacity:.88;animation:ace-pointer-drag 820ms ease-in-out infinite alternate}
   @keyframes ace-pointer-click{0%{opacity:.95;transform:scale(.28)}72%{opacity:.72;transform:scale(1.08)}100%{opacity:.2;transform:scale(1.28)}}
   @keyframes ace-pointer-scroll{0%{opacity:.9;transform:scale(.55) rotate(0deg)}100%{opacity:.28;transform:scale(1.08) rotate(90deg)}}
   @keyframes ace-pointer-drag{from{transform:scale(.62)}to{transform:scale(.92)}}
   @media (prefers-reduced-motion:reduce){.ace-pointer-ring{animation:none!important;opacity:.8!important;transform:scale(.82)!important}}
  `;
  const wrap=document.createElement('div');
  wrap.className='ace-pointer-wrap';
  const ring=document.createElement('div');
  ring.className='ace-pointer-ring';
  const svg=document.createElementNS('http://www.w3.org/2000/svg','svg');
  svg.setAttribute('class','ace-pointer-cursor');
  svg.setAttribute('viewBox','0 0 24 30');
  svg.setAttribute('aria-hidden','true');
  const path=document.createElementNS('http://www.w3.org/2000/svg','path');
  path.setAttribute('d','M2 2L2.4 22.5L7.9 17.1L12.7 27.1L17.2 24.9L12.5 15.2L21.4 15.1Z');
  path.setAttribute('fill','#6366f1');
  path.setAttribute('stroke','#ffffff');
  path.setAttribute('stroke-width','2');
  path.setAttribute('stroke-linejoin','round');
  svg.append(path);
  const badge=document.createElement('div');
  badge.className='ace-pointer-badge';
  badge.textContent='AI';
  wrap.append(ring,svg,badge);
  shadow.append(style,wrap);
  root.append(host);
 }
 // A host left behind by an older script can contain an inline `all`
 // shorthand that resets the placement longhands. Remove only that legacy
 // style; preserving current longhands lets drag updates animate from the
 // previous exact coordinate.
 if(host.style.getPropertyValue('all'))host.removeAttribute('style');
 const reduced=typeof matchMedia==='function'&&matchMedia('(prefers-reduced-motion: reduce)').matches;
 const transition=reduced?'none':action==='drag'
  ?'left 180ms linear, top 180ms linear, opacity 180ms ease-out'
  :'opacity 180ms ease-out';
 host.style.setProperty('transition',transition,'important');
 const critical={display:'block',position:'fixed',left:`${x}px`,top:`${y}px`,width:'0',height:'0',margin:'0',padding:'0',border:'0',overflow:'visible','pointer-events':'none','user-select':'none','-webkit-user-select':'none',visibility:'visible',opacity:'1','z-index':'2147483647'};
 for(const [name,value]of Object.entries(critical))host.style.setProperty(name,value,'important');
 const wrap=host.shadowRoot.querySelector('.ace-pointer-wrap');
 wrap.removeAttribute('data-action');
 void wrap.getBoundingClientRect();
 wrap.setAttribute('data-action',action);
 const sequence=String((Number(host.dataset.acePointerSequence)||0)+1);
 host.dataset.acePointerSequence=sequence;
 const duration=action==='hover'||action==='drag'?2200:1800;
 setTimeout(()=>{if(host.isConnected&&host.dataset.acePointerSequence===sequence){host.style.setProperty('opacity','0','important');wrap.removeAttribute('data-action');}},duration);
 return {ok:true,action,x,y};
})JS";
}

std::string invoke_pointer_renderer(const std::string& payload) {
    return "(" + std::string(pointer_renderer_source()) + ")(" + payload + ")";
}

} // namespace

std::string agent_browser_pointer_script(
    double x,
    double y,
    const std::string& action) {
    const std::string safe_action =
        action == "click" || action == "scroll" || action == "drag"
        ? action : "hover";
    const nlohmann::json payload{
        {"x", x},
        {"y", y},
        {"action", safe_action},
    };
    return invoke_pointer_renderer(payload.dump());
}

std::string agent_browser_evaluate_pointer_observer_script(bool install) {
    constexpr const char* key = "__acecodeEvaluatePointerObserverV1";
    if (!install) {
        return "(() => {const key='" + std::string(key) +
            R"JS(';const state=globalThis[key];if(state&&typeof state.remove==='function')state.remove();return {ok:true,removed:Boolean(state)};})())JS";
    }

    return "(() => {\n const key='" + std::string(key) +
        "';\n const previous=globalThis[key];"
        "if(previous&&typeof previous.remove==='function')previous.remove();"
        "\n const render=" + pointer_renderer_source() + R"JS(;
 const types=['pointerdown','mousedown','click','dblclick','contextmenu','pointermove','mousemove','wheel'];
 let lastClick=null;
 const onEvent=(event)=>{
  if(!event||event.isTrusted!==false)return;
  const type=String(event.type||'');
  const action=type==='wheel'?'scroll'
   :(type==='pointermove'||type==='mousemove')
    ?(Number(event.buttons||0)?'drag':'hover')
    :'click';
  let x=Number(event.clientX);
  let y=Number(event.clientY);
  if(!Number.isFinite(x)||!Number.isFinite(y))return;
  const path=typeof event.composedPath==='function'?event.composedPath():[];
  const target=path[0]||event.target;
  const rect=target&&typeof target.getBoundingClientRect==='function'
   ?target.getBoundingClientRect():null;
  const zeroOutsideTarget=x===0&&y===0&&Number(event.detail||0)===0&&rect
   &&!(rect.left<=0&&rect.right>=0&&rect.top<=0&&rect.bottom>=0);
  if(action==='click'&&zeroOutsideTarget){
   if(rect.width<=0||rect.height<=0||rect.right<=0||rect.bottom<=0
      ||rect.left>=innerWidth||rect.top>=innerHeight)return;
   x=Math.max(0,Math.min(innerWidth,rect.left+rect.width/2));
   y=Math.max(0,Math.min(innerHeight,rect.top+rect.height/2));
  }
  if(x<0||y<0||x>innerWidth||y>innerHeight)return;
  const now=typeof performance==='object'?performance.now():Date.now();
  if(action==='click'&&lastClick&&Math.abs(lastClick.x-x)<.5
     &&Math.abs(lastClick.y-y)<.5&&now-lastClick.at<180)return;
  if(action==='click')lastClick={x,y,at:now};
  render({x,y,action});
 };
 for(const type of types)globalThis.addEventListener(type,onEvent,true);
 let expiry=0;
 const remove=()=>{
  if(expiry)clearTimeout(expiry);
  for(const type of types)globalThis.removeEventListener(type,onEvent,true);
  if(globalThis[key]?.remove===remove){
   try{delete globalThis[key];}catch(_){globalThis[key]=undefined;}
  }
 };
 Object.defineProperty(globalThis,key,{value:{remove},configurable:true});
 expiry=setTimeout(remove,20000);
 return {ok:true,installed:true};
})())JS";
}

} // namespace acecode::agent_browser
