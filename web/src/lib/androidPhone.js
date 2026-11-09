import { useEffect, useState } from 'react';
import { connection } from './connection.js';

let nextId = 0;
const waiting = new Map();
let listening = null;

export function phoneRequest(action, fields = {}, bridge = globalThis.window?.acePhone) {
  if (!bridge?.postMessage) return Promise.reject(new Error('Android bridge unavailable'));
  if (listening !== bridge) {
    listening = bridge;
    bridge.onmessage = (event) => {
      let data;
      try { data = JSON.parse(event.data); } catch { return; }
      waiting.get(data.id)?.(data);
    };
  }
  return new Promise((resolve, reject) => {
    const id = String(++nextId);
    const finish = (data) => {
      clearTimeout(timer);
      waiting.delete(id);
      if (data.error) reject(new Error(data.error)); else resolve(data);
    };
    const timer = setTimeout(() => finish({ error: 'Android request timed out' }), 5000);
    waiting.set(id, finish);
    try { bridge.postMessage(JSON.stringify({ ...fields, id, action })); }
    catch (error) { finish({ error: error.message }); }
  });
}

export function useAndroidPetActivity(waitingForUser) {
  const [activity, setActivity] = useState('idle');
  useEffect(() => {
    if (!window.acePhone) return undefined;
    const running = new Set();
    let reset;
    function update(id, busy) {
      if (!id) return;
      if (busy) running.add(id); else running.delete(id);
    }
    const onMessage = ({ detail: message }) => {
      const data = message?.payload || {};
      const wasBusy = running.size > 0;
      if (message?.type === 'session_status_snapshot') {
        for (const row of data.sessions || []) update(row.session_id, row.busy);
      } else if (['session_status', 'busy_changed'].includes(message?.type)) {
        update(message.session_id || data.session_id, data.busy);
      } else return;
      if (running.size) { clearTimeout(reset); setActivity('working'); }
      else if (wasBusy) { setActivity('done'); reset = setTimeout(() => setActivity('idle'), 4000); }
    };
    const onDisconnect = () => { running.clear(); clearTimeout(reset); setActivity('idle'); };
    connection.addEventListener('message', onMessage);
    connection.addEventListener('disconnect', onDisconnect);
    return () => {
      clearTimeout(reset);
      connection.removeEventListener('message', onMessage);
      connection.removeEventListener('disconnect', onDisconnect);
    };
  }, []);
  useEffect(() => {
    if (window.acePhone) void phoneRequest('activity', { state: waitingForUser ? 'waiting' : activity }).catch(() => {});
  }, [waitingForUser, activity]);
}

export function usePhoneApproval(requests, decide) {
  const pending = requests.find(row => row.status === 'pending' &&
    ['mcp_phone_open', 'mcp_phone_tap'].includes(row.tool));
  useEffect(() => {
    if (!window.acePhone) return undefined;
    const request = pending ? { requestId: pending.request_id, tool: pending.tool, args: pending.args } : null;
    void phoneRequest('approval', { request }).catch(() => {});
    const answer = ({ detail }) => {
      if (pending && detail?.requestId === pending.request_id && ['allow', 'deny'].includes(detail.choice)) {
        decide(pending, detail.choice);
      }
    };
    window.addEventListener('ace-phone-decision', answer);
    return () => window.removeEventListener('ace-phone-decision', answer);
  }, [pending, decide]);
}
