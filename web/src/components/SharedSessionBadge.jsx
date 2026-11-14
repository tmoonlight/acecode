import { useSyncExternalStore } from 'react';
import { api } from '../lib/api.js';
import { VsIcon } from './Icon.jsx';

let shared = new Set();
let loading;
let expiry;
const listeners = new Set();
function reload() {
  if (loading) return loading;
  loading = api.listShares().then(({ shares = [] }) => {
    const now = Date.now() / 1000;
    const active = shares.filter((share) => share.active && share.expires_at > now);
    shared = new Set(active.map((share) => share.session_id));
    listeners.forEach((listener) => listener());
    clearTimeout(expiry);
    if (active.length && listeners.size) expiry = setTimeout(reload, Math.min(2147483647, Math.max(1000, (Math.min(...active.map((share) => share.expires_at)) - now) * 1000)));
  }).catch(() => {}).finally(() => { loading = null; });
  return loading;
}
function subscribe(listener) {
  if (!listeners.size) { window.addEventListener('acecode:shares-changed', reload); void reload(); }
  listeners.add(listener);
  return () => { listeners.delete(listener); if (!listeners.size) { window.removeEventListener('acecode:shares-changed', reload); clearTimeout(expiry); } };
}
const snapshot = () => shared;
export function SharedSessionBadge({ sessionId }) {
  const sessions = useSyncExternalStore(subscribe, snapshot);
  return sessions.has(sessionId) ? <VsIcon name="radar" size={12} alt="已分享" className="text-fg-mute shrink-0" /> : null;
}
