import assert from 'node:assert/strict';
import {
  appearanceBootstrapPreferences,
  appearancePreferencesToApi,
  createAppearancePersistenceController,
  effectiveAppearanceTheme,
  initialAppearancePreferences,
  normalizeAppearancePreferences,
  parseAppearancePreferences,
  systemThemeFallback,
} from './appearancePreferences.js';

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('appearance defaults preserve system preference, blue, and medium', () => {
  const darkScope = { matchMedia: () => ({ matches: true }) };
  assert.equal(systemThemeFallback(darkScope), 'dark');
  assert.equal(effectiveAppearanceTheme('system', darkScope), 'dark');
  assert.deepEqual(normalizeAppearancePreferences({}, darkScope), {
    theme: 'system',
    colorTheme: 'blue',
    fontSize: 'medium',
  });
});

await run('desktop bootstrap is normalized before first render', () => {
  const scope = {
    __ACECODE_APPEARANCE__: {
      theme: 'light',
      color_theme: 'orange',
      font_size: 'large',
    },
    matchMedia: () => ({ matches: true }),
  };
  assert.deepEqual(initialAppearancePreferences(scope), {
    theme: 'light',
    colorTheme: 'orange',
    fontSize: 'large',
  });
  assert.deepEqual(appearanceBootstrapPreferences(scope), {
    theme: 'light',
    colorTheme: 'orange',
    fontSize: 'large',
  });
});

await run('canonical parser rejects incomplete legacy daemon payloads', () => {
  assert.equal(parseAppearancePreferences({ show_acecode_avatar: false }), null);
  assert.equal(parseAppearancePreferences({
    theme: 'dark',
    color_theme: 'green',
    font_size: 'large',
  }), null);
});

await run('API serialization sends a complete compatibility snapshot', () => {
  assert.deepEqual(appearancePreferencesToApi({
    theme: 'dark',
    colorTheme: 'orange',
    fontSize: 'small',
  }), {
    show_acecode_avatar: false,
    theme: 'dark',
    color_theme: 'orange',
    font_size: 'small',
  });
});

await run('canonical restore wins only before a local user mutation', async () => {
  const applied = [];
  const controller = createAppearancePersistenceController({
    initial: { theme: 'light', colorTheme: 'blue', fontSize: 'medium' },
    apply: (value) => applied.push(value),
    save: async (value) => value,
  });
  assert.equal(controller.restore({
    theme: 'dark',
    color_theme: 'orange',
    font_size: 'large',
  }), true);
  await controller.change({ fontSize: 'small' });
  assert.equal(controller.restore({
    theme: 'light',
    color_theme: 'blue',
    font_size: 'medium',
  }), false);
  assert.deepEqual(controller.current(), {
    theme: 'dark',
    colorTheme: 'orange',
    fontSize: 'small',
  });
  assert.equal(applied.length, 3);
});

await run('color and font changes preserve a canonical system theme preference', async () => {
  const calls = [];
  const controller = createAppearancePersistenceController({
    initial: { theme: 'system', colorTheme: 'blue', fontSize: 'medium' },
    apply: () => {},
    save: async (value) => {
      calls.push(value);
      return value;
    },
  });
  await controller.change({ colorTheme: 'orange', fontSize: 'large' });
  assert.equal(calls[0].theme, 'system');
  assert.deepEqual(controller.confirmed(), {
    theme: 'system',
    colorTheme: 'orange',
    fontSize: 'large',
  });
});

await run('failed latest save rolls back to the last confirmed appearance', async () => {
  const applied = [];
  const errors = [];
  const controller = createAppearancePersistenceController({
    initial: { theme: 'light', colorTheme: 'blue', fontSize: 'medium' },
    apply: (value) => applied.push(value),
    save: async () => { throw new Error('disk full'); },
    onError: (error) => errors.push(error.message),
  });
  await controller.change({ theme: 'dark', colorTheme: 'orange' });
  assert.deepEqual(applied, [
    { theme: 'dark', colorTheme: 'orange', fontSize: 'medium' },
    { theme: 'light', colorTheme: 'blue', fontSize: 'medium' },
  ]);
  assert.deepEqual(errors, ['disk full']);
});

await run('rapid changes serialize saves and keep the newest snapshot', async () => {
  const calls = [];
  const releases = [];
  const controller = createAppearancePersistenceController({
    initial: { theme: 'light', colorTheme: 'blue', fontSize: 'medium' },
    apply: () => {},
    save: (value) => {
      calls.push(value);
      return new Promise((resolve) => releases.push(() => resolve(value)));
    },
  });
  controller.change({ theme: 'dark' });
  controller.change({ colorTheme: 'orange' });
  await Promise.resolve();
  assert.equal(calls.length, 1);
  releases.shift()();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(calls.length, 2);
  releases.shift()();
  await controller.idle();
  assert.deepEqual(controller.confirmed(), {
    theme: 'dark',
    colorTheme: 'orange',
    fontSize: 'medium',
  });
});
