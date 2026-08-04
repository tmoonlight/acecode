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
    configured_bind: '0.0.0.0',
    effective_bind: '0.0.0.0',
    port: 28080,
    connections: [
      {
        host: 'ACE-PC',
        kind: 'computer_name',
        url: 'http://ACE-PC:28080/?token=abc',
      },
      { host: '192.168.1.8', url: 'http://192.168.1.8:28080/?token=abc' },
      { host: 'duplicate', url: 'http://192.168.1.8:28080/?token=abc' },
      { host: 'bad', url: 'javascript:alert(1)' },
    ],
  });

  assert.equal(state.configuredEnabled, true);
  assert.equal(state.effectiveEnabled, true);
  assert.equal(state.port, 28080);
  assert.deepEqual(state.connections, [
    {
      host: 'ACE-PC',
      kind: 'computer_name',
      url: 'http://ACE-PC:28080/?token=abc',
    },
    {
      host: '192.168.1.8',
      kind: 'network_address',
      url: 'http://192.168.1.8:28080/?token=abc',
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

await run('remote Web polling tolerates listener downtime and waits for effective bind', async () => {
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

console.log('remoteWeb tests passed');
