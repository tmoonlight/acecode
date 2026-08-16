import { composerFileIdentity } from './composerFileTransfer.js';

export function createComposerAttachmentReservations() {
  return {
    identityByLocalId: new Map(),
    localIdByIdentity: new Map(),
    reservationByLocalId: new Map(),
  };
}

function defaultLocalId(_file, index) {
  return `local-${Date.now()}-${index}-${Math.random().toString(16).slice(2)}`;
}

export function reserveComposerAttachmentFiles(
  reservations,
  files,
  createLocalId = defaultLocalId,
) {
  if (!reservations?.identityByLocalId || !reservations?.localIdByIdentity) return [];
  const accepted = [];
  for (const [index, file] of Array.from(files || []).filter(Boolean).entries()) {
    const identity = composerFileIdentity(file);
    if (reservations.localIdByIdentity.has(identity)) continue;
    const localId = String(createLocalId(file, index) || '');
    if (!localId || reservations.identityByLocalId.has(localId)) continue;
    const reservation = { file, identity, localId };
    reservations.identityByLocalId.set(localId, identity);
    reservations.localIdByIdentity.set(identity, localId);
    reservations.reservationByLocalId?.set(localId, reservation);
    accepted.push(reservation);
  }
  return accepted;
}

export function composerAttachmentFilesForLocalIds(reservations, localIds) {
  if (!reservations?.reservationByLocalId) return [];
  return Array.from(localIds || [])
    .map((localId) => reservations.reservationByLocalId.get(String(localId || '')))
    .filter(Boolean);
}

export function releaseComposerAttachmentFile(reservations, localId) {
  const key = String(localId || '');
  if (!key || !reservations?.identityByLocalId || !reservations?.localIdByIdentity) return false;
  const identity = reservations.identityByLocalId.get(key);
  if (!identity) return false;
  reservations.identityByLocalId.delete(key);
  reservations.reservationByLocalId?.delete(key);
  if (reservations.localIdByIdentity.get(identity) === key) {
    reservations.localIdByIdentity.delete(identity);
  }
  return true;
}

export function clearComposerAttachmentReservations(reservations) {
  reservations?.identityByLocalId?.clear();
  reservations?.localIdByIdentity?.clear();
  reservations?.reservationByLocalId?.clear();
}
