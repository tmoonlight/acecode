// One daemon event updates both the saved list and the active composer.
// Reconnecting also reloads local state in case background discovery completed
// before the socket opened. Neither path performs another remote probe.
export function subscribeModelProfileUpdates(connection, onChange) {
  const onMessage = (event) => {
    if (event.detail?.type === 'model_profiles_updated') onChange();
  };
  connection.addEventListener('message', onMessage);
  connection.addEventListener('open', onChange);
  return () => {
    connection.removeEventListener('message', onMessage);
    connection.removeEventListener('open', onChange);
  };
}

// The POST only queues work. Callers can start it without waiting for discovery.
export async function requestSavedModelReasoningSync(apiClient) {
  try {
    await apiClient.refreshModelReasoning();
  } catch {
    // Missing metadata must not block the composer, including on older daemons.
  }
}

// Both failures are deliberately silent.
// Reload in parallel so an unreachable provider never delays the local list.
export async function refreshSavedModelReasoning(apiClient, reload) {
  await Promise.allSettled([
    requestSavedModelReasoningSync(apiClient),
    Promise.resolve().then(reload),
  ]);
}
