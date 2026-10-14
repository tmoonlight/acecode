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
    get(key, startStream) {
      if (!key) return createSideChatController({ startStream, now });
      let entry = sessions.get(key);
      if (!entry) {
        entry = { startStream };
        entry.controller = createSideChatController({
          now,
          startStream: (options) => entry.startStream(options),
        });
        sessions.set(key, entry);
      }
      // A session can be restored after its API credentials have changed.
      entry.startStream = startStream;
      return entry.controller;
    },
  };
}

export const sideChatSessions = createSideChatSessionStore();
