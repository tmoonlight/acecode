import assert from 'node:assert/strict';
import {
  normalizeRemoteWebState,
  remoteWebModeReady,
  remoteWebOriginSurvivesLocalMode,
  selectRemoteWebConnection,
  waitForRemoteWebMode,
} from './remoteWeb.js';

function run(name, fn) {
  try {
    const result = fn();
    if (result && typeof result.then === 'function') {
      return result.then(
        () => console.log(`[pass] ${name}`),
        (error) => { console.error(`[fail] ${name}`); throw error; },
      );
    }
    console.log(`[pass] ${name}`);
    return undefined;
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('remote Web state normalizes booleans and safe connection candidates', () => {
  const state = normalizeRemoteWebState({
    enabled: true,
    effective_enabled: true,
    configured_bind: '127.0.0.1',
    effective_bind: '0.0.0.0',
    daemon_bind: '127.0.0.1',
    daemon_port: 28080,
    proxy_bind: '0.0.0.0',
    proxy_state: 'running',
    proxy_pid: 4242,
    proxy_ipv6: true,
    port: 28081,
    connections: [
      {
        host: 'ACE-PC',
        kind: 'computer_name',
        url: 'http://ACE-PC:28081/?token=abc',
      },
      { host: '192.168.1.8', url: 'http://192.168.1.8:28081/?token=abc' },
      { host: 'duplicate', url: 'http://192.168.1.8:28081/?token=abc' },
      { host: 'bad', url: 'javascript:alert(1)' },
    ],
  });

  assert.equal(state.configuredEnabled, true);
  assert.equal(state.effectiveEnabled, true);
  assert.equal(state.port, 28081);
  assert.equal(state.daemonBind, '127.0.0.1');
  assert.equal(state.daemonPort, 28080);
  assert.equal(state.proxyState, 'running');
  assert.equal(state.proxyPid, 4242);
  assert.equal(state.proxyIpv6, true);
  assert.deepEqual(state.connections, [
    {
      host: 'ACE-PC',
      kind: 'computer_name',
      url: 'http://ACE-PC:28081/?token=abc',
    },
    {
      host: '192.168.1.8',
      kind: 'network_address',
      url: 'http://192.168.1.8:28081/?token=abc',
    },
  ]);
});

run('remote Web connection defaults to the first computer-name URL', () => {
  const state = normalizeRemoteWebState({
    connections: [
      {
        host: 'ACE-PC',
        kind: 'computer_name',
        url: 'http://ACE-PC:28080/?token=a',
      },
      { host: 'vpn', url: 'https://vpn:443/?token=a' },
    ],
  });
  assert.equal(
    selectRemoteWebConnection(state, 'https://vpn:443/?token=a')?.host,
    'vpn',
  );
  assert.equal(selectRemoteWebConnection(state, 'missing')?.host, 'ACE-PC');
  assert.equal(
    selectRemoteWebConnection(state)?.kind,
    'computer_name',
  );
  assert.deepEqual(normalizeRemoteWebState(state), state);
  assert.equal(remoteWebModeReady({
    ...state,
    configuredEnabled: true,
    effectiveEnabled: false,
  }, true), false);
  assert.equal(remoteWebOriginSurvivesLocalMode('127.0.0.1'), true);
  assert.equal(remoteWebOriginSurvivesLocalMode('ACE.localhost'), true);
  assert.equal(remoteWebOriginSurvivesLocalMode('192.168.1.8'), false);
});

await run('remote Web polling waits for effective proxy readiness', async () => {
  const responses = [
    new Error('connection refused'),
    {
      configured_enabled: true,
      effective_enabled: false,
      applying: true,
    },
    {
      configured_enabled: true,
      effective_enabled: true,
      applying: false,
      effective_bind: '0.0.0.0',
    },
  ];
  const state = await waitForRemoteWebMode(
    {
      getRemoteWeb: async () => {
        const value = responses.shift();
        if (value instanceof Error) throw value;
        return value;
      },
    },
    true,
    { attempts: 3, intervalMs: 0, wait: async () => {} },
  );
  assert.equal(remoteWebModeReady(state, true), true);
  assert.equal(state.effectiveBind, '0.0.0.0');
});

await run('remote disable treats the expected remote-origin disconnect as ready', async () => {
  const state = await waitForRemoteWebMode(
    {
      getRemoteWeb: async () => {
        throw new TypeError('fetch failed');
      },
    },
    false,
    {
      initialState: {
        configured_enabled: false,
        effective_enabled: true,
        applying: true,
      },
      attempts: 1,
      intervalMs: 0,
      acceptDisconnectWhenDisabling: true,
      wait: async () => {},
    },
  );
  assert.equal(remoteWebModeReady(state, false), true);
  assert.equal(state.effectiveBind, '127.0.0.1');
});

await run('remote Web polling surfaces the proxy runtime error', async () => {
  await assert.rejects(
    waitForRemoteWebMode(
      {
        getRemoteWeb: async () => ({
          configured_enabled: false,
          effective_enabled: true,
          proxy_state: 'failed',
          error: 'failed to stop remote Web proxy process',
        }),
      },
      false,
      { attempts: 1, intervalMs: 0, wait: async () => {} },
    ),
    /failed to stop remote Web proxy process/,
  );
});

console.log('remoteWeb tests passed');
