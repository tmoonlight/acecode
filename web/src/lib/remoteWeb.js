const DEFAULT_REMOTE_WEB_STATE = Object.freeze({
  enabled: false,
  configuredEnabled: false,
  effectiveEnabled: false,
  configuredBind: '127.0.0.1',
  effectiveBind: '127.0.0.1',
  applying: false,
  port: 0,
  connections: [],
});

function normalizedConnection(value) {
  if (!value || typeof value !== 'object') return null;
  const host = String(value.host || '').trim();
  const url = String(value.url || '').trim();
  if (!host || !/^https?:\/\/[^/\s]+(?:\/|$)/i.test(url)) return null;
  const kind = value.kind === 'computer_name'
    ? 'computer_name'
    : 'network_address';
  return { host, kind, url };
}

export function normalizeRemoteWebState(value) {
  if (!value || typeof value !== 'object') {
    return { ...DEFAULT_REMOTE_WEB_STATE, connections: [] };
  }
  const configuredValue = value.configured_enabled
    ?? value.configuredEnabled
    ?? value.enabled;
  const configuredEnabled = configuredValue === true;
  const effectiveValue = value.effective_enabled
    ?? value.effectiveEnabled;
  const effectiveEnabled = effectiveValue == null
    ? configuredEnabled
    : effectiveValue === true;
  const portValue = Number(value.port);
  const seen = new Set();
  const connections = [];
  for (const item of Array.isArray(value.connections) ? value.connections : []) {
    const connection = normalizedConnection(item);
    if (!connection || seen.has(connection.url)) continue;
    seen.add(connection.url);
    connections.push(connection);
  }
  return {
    enabled: configuredEnabled,
    configuredEnabled,
    effectiveEnabled,
    configuredBind: String(
      value.configured_bind || value.configuredBind || '127.0.0.1',
    ),
    effectiveBind: String(
      value.effective_bind || value.effectiveBind || '127.0.0.1',
    ),
    applying: value.applying === true,
    port: Number.isInteger(portValue) && portValue > 0 ? portValue : 0,
    connections,
  };
}

export function selectRemoteWebConnection(state, selectedUrl = '') {
  const normalized = normalizeRemoteWebState(state);
  return normalized.connections.find(
    (connection) => connection.url === selectedUrl,
  ) || normalized.connections[0] || null;
}

export function remoteWebModeReady(state, expectedEnabled) {
  const normalized = normalizeRemoteWebState(state);
  return !normalized.applying
    && normalized.configuredEnabled === !!expectedEnabled
    && normalized.effectiveEnabled === !!expectedEnabled;
}

export function remoteWebOriginSurvivesLocalMode(hostname) {
  let host = String(hostname || '').trim().toLowerCase();
  if (host.startsWith('[') && host.endsWith(']')) {
    host = host.slice(1, -1);
  }
  return host === '127.0.0.1'
    || host === 'localhost'
    || host.endsWith('.localhost');
}

export async function waitForRemoteWebMode(
  apiClient,
  expectedEnabled,
  {
    initialState = null,
    attempts = 40,
    intervalMs = 125,
    acceptDisconnectWhenDisabling = false,
    wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms)),
  } = {},
) {
  let latest = normalizeRemoteWebState(initialState);
  if (remoteWebModeReady(latest, expectedEnabled)) return latest;

  let lastError = null;
  for (let attempt = 0; attempt < attempts; attempt += 1) {
    await wait(intervalMs);
    try {
      latest = normalizeRemoteWebState(await apiClient.getRemoteWeb());
      lastError = null;
      if (remoteWebModeReady(latest, expectedEnabled)) return latest;
    } catch (error) {
      // The listener is briefly absent between the old and new bind.
      lastError = error;
      if (!expectedEnabled
          && acceptDisconnectWhenDisabling
          && !Number.isInteger(error?.status)) {
        // A page opened through the remote address becomes unreachable by
        // design as soon as the listener moves back to loopback.
        return normalizeRemoteWebState({
          ...latest,
          enabled: false,
          configuredEnabled: false,
          effectiveEnabled: false,
          configuredBind: '127.0.0.1',
          effectiveBind: '127.0.0.1',
          applying: false,
          connections: [],
        });
      }
    }
  }

  const timeout = new Error(
    lastError?.message || '远程 Web 监听地址切换超时',
  );
  timeout.state = latest;
  throw timeout;
}
