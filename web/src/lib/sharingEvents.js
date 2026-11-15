import { extractInviteLink } from './inviteLink.js';
export const SHARE_SESSION_EVENT = 'acecode:share-session';
export const INVITE_LINK_EVENT = 'acecode:invite-link';
export function openSessionSharing(detail) {
  window.dispatchEvent(new CustomEvent(SHARE_SESSION_EVENT, { detail }));
}
export function offerInvitation(text, onCancel) {
  const link = extractInviteLink(text);
  if (!link) return false;
  window.dispatchEvent(new CustomEvent(INVITE_LINK_EVENT, { detail: { link, onCancel } }));
  return true;
}
