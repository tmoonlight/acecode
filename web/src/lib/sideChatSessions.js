import { createSideChatController } from './sideChatController.js';

export function sideChatSessionKey(ref = {}) {
  const id = ref?.sessionId || ref?.session_id || ref?.id;
  if (!id) return '';
  return JSON.stringify([ref?.port || '', ref?.workspaceHash || ref?.workspace_hash || '', id]);
}

// Temporary side conversations belong to the browser page, independently of
// ChatView mounts. Only the retention preference is persisted to storage.
export function createSideChatSessionStore({ now = Date.now } = {}) {
  const sessions = new Map();
  return {
    get(key, startStream, sendToMain, uploadAttachment) {
      if (!key) return createSideChatController({ startStream, sendToMain, uploadAttachment, now });
      let entry = sessions.get(key);
      if (!entry) {
        entry = { startStream, sendToMain, uploadAttachment };
        entry.controller = createSideChatController({
          now,
          startStream: (options) => entry.startStream(options),
          sendToMain: (message) => entry.sendToMain(message),
          uploadAttachment: (file) => entry.uploadAttachment(file),
        });
        sessions.set(key, entry);
      }
      // A session can be restored after its API credentials have changed.
      entry.startStream = startStream;
      entry.sendToMain = sendToMain;
      entry.uploadAttachment = uploadAttachment;
      return entry.controller;
    },
  };
}

export const sideChatSessions = createSideChatSessionStore();
