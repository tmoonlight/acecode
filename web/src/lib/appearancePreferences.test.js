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

// 主题风格与动效开关的默认值:空风格 = 主题的第一个风格,动效默认开。
const THEME_STYLE_DEFAULTS = Object.freeze({ themeVariant: '', themeEffects: true });

async function run(name, fn) {
  try {
    await fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

await run('failed automatic theme persistence silently restores the original new or upgraded appearance', async () => {
  for (const colorTheme of ['blue', 'orange', 'ai-existing']) {
    const initial = { theme: 'system', colorTheme, fontSize: 'large', sidebarSessionTime: false, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS };
    const applied = [], errors = [], saves = [];
    const expected = appearancePreferencesToApi(initial);
    const controller = createAppearancePersistenceController({ initial,
      apply: (value) => applied.push(value), save: async (payload) => {
        saves.push(payload);
        throw new Error('disk full');
      },
      onError: (error) => errors.push(error),
    });
    await controller.change({ colorTheme: 'national-day-2026' }, { silent: true, expectedAppearance: expected });
    assert.deepEqual(saves[0].expected_appearance, expected);
    assert.deepEqual(controller.current(), initial);
    assert.deepEqual(applied.at(-1), initial);
    assert.deepEqual(errors, []);
  }
});

await run('appearance defaults preserve system preference, blue, and medium', () => {
  const darkScope = { matchMedia: () => ({ matches: true }) };
  assert.equal(systemThemeFallback(darkScope), 'dark');
  assert.equal(effectiveAppearanceTheme('system', darkScope), 'dark');
  assert.deepEqual(normalizeAppearancePreferences({}, darkScope), {
    theme: 'system',
    colorTheme: 'blue',
    fontSize: 'medium',
    // 侧栏时间是产品默认开的显示项:空输入必须归一成 true,否则升级用户
    // 一进来就发现时间列没了。
    sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS,
  });
  // 只有显式 false 才关;缺键、null、非布尔值都按开处理,免得旧 daemon 的
  // 半截报文把这一列意外关掉。
  assert.equal(
    normalizeAppearancePreferences({ sidebar_session_time: false }, darkScope).sidebarSessionTime,
    false,
  );
  assert.equal(
    normalizeAppearancePreferences({ sidebar_session_time: null }, darkScope).sidebarSessionTime,
    true,
  );
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
    sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS,
  });
  assert.deepEqual(appearanceBootstrapPreferences(scope), {
    theme: 'light',
    colorTheme: 'orange',
    fontSize: 'large',
    sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS,
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
    sidebar_session_time: true,
    message_auto_collapse: true,
    theme_variant: '',
    theme_effects: true,
  });
});

await run('startup restores each saved theme without rewriting preferences', () => {
  for (const colorTheme of ['blue', 'orange', 'national-day-2026', 'eva-01', 'ai-existing']) {
    const applied = [], saves = [];
    const controller = createAppearancePersistenceController({
      initial: { theme: 'system', colorTheme: 'blue', fontSize: 'medium' },
      apply: (value) => applied.push(value),
      save: async (value) => { saves.push(value); return value; },
    });
    assert.equal(controller.restore({ theme: 'dark', color_theme: colorTheme, font_size: 'large' }), true);
    assert.equal(controller.current().colorTheme, colorTheme);
    assert.equal(controller.current().theme, 'dark');
    assert.equal(controller.current().fontSize, 'large');
    assert.equal(applied.at(-1).colorTheme, colorTheme);
    assert.deepEqual(saves, []);
  }
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
    sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS,
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
    sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS,
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
    { theme: 'dark', colorTheme: 'orange', fontSize: 'medium', sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS },
    { theme: 'light', colorTheme: 'blue', fontSize: 'medium', sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS },
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
    sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS,
  });
});

const deferred = () => { let resolve; const promise = new Promise((r) => { resolve = r; }); return { promise, resolve }; };
const deletionTick = () => new Promise((resolve) => setImmediate(resolve));
const localAppearance = { theme: 'dark', colorTheme: 'ai-night', fontSize: 'medium', sidebarSessionTime: true, messageAutoCollapse: true, ...THEME_STYLE_DEFAULTS };

await run('消息自动折叠缺省开启，关闭值经启动注入、API、其它偏好修改后保持', async () => {
  const canonical = { theme: 'system', color_theme: 'blue', font_size: 'medium', message_auto_collapse: false };
  assert.equal(normalizeAppearancePreferences({}).messageAutoCollapse, true);
  assert.equal(normalizeAppearancePreferences({ message_auto_collapse: 'false' }).messageAutoCollapse, true);
  assert.equal(initialAppearancePreferences({ __ACECODE_APPEARANCE__: canonical }).messageAutoCollapse, false);
  assert.equal(parseAppearancePreferences(canonical).messageAutoCollapse, false);
  const saves = [];
  const controller = createAppearancePersistenceController({
    initial: {}, apply: () => {}, save: async (value) => { saves.push(value); return value; },
  });
  assert.equal(controller.restore(canonical), true);
  await controller.change({ fontSize: 'large' });
  assert.equal(saves[0].message_auto_collapse, false);
  assert.equal(controller.confirmed().messageAutoCollapse, false);
  await controller.change({ messageAutoCollapse: true });
  assert.equal(saves.at(-1).message_auto_collapse, true);
});

await run('消息折叠切换立即更新，保存失败恢复确认值，连续切换按最后选择落盘', async () => {
  const applied = [];
  let fail = true;
  const controller = createAppearancePersistenceController({
    initial: {}, apply: (value) => applied.push(value.messageAutoCollapse),
    save: async (value) => { if (fail) throw new Error('disk full'); return value; },
  });
  const rejected = controller.change({ messageAutoCollapse: false });
  assert.deepEqual(applied, [false]);
  await rejected;
  assert.deepEqual(applied, [false, true]);
  fail = false;
  controller.change({ messageAutoCollapse: false });
  controller.change({ messageAutoCollapse: true });
  controller.change({ messageAutoCollapse: false });
  await controller.idle();
  assert.equal(controller.confirmed().messageAutoCollapse, false);
});

await run('deleting the current theme waits for older writes and rewrites later snapshots referencing it', async () => {
  const firstSave = deferred(), deletion = deferred(), events = [];
  const controller = createAppearancePersistenceController({ initial: localAppearance, apply: () => {}, save: async (value) => {
    events.push(['save', value.color_theme, value.font_size]);
    if (events.length === 1) await firstSave.promise;
    return value;
  } });
  controller.change({ fontSize: 'small' });
  const deleting = controller.removeColorTheme('ai-night', async () => {
    events.push(['delete']);
    await deletion.promise;
    return { id: 'ai-night', deleted: true, ui_preferences: { theme: 'dark', color_theme: 'blue', font_size: 'small' } };
  });
  controller.change({ fontSize: 'large' });
  await deletionTick();
  assert.deepEqual(events, [['save', 'ai-night', 'small']]);
  firstSave.resolve();
  await deletionTick();
  assert.deepEqual(events.at(-1), ['delete']);
  deletion.resolve();
  await deleting;
  await controller.idle();
  assert.deepEqual(events.at(-1), ['save', 'blue', 'large']);
  assert.equal(controller.current().colorTheme, 'blue');
  assert.equal(controller.current().fontSize, 'large');
  assert.equal(controller.confirmed().colorTheme, 'blue');
});

await run('later manual theme choices survive deletion and a subsequent save failure rolls back to blue', async () => {
  for (const failSave of [false, true]) {
    const deleting = deferred(), applied = [];
    const controller = createAppearancePersistenceController({ initial: localAppearance, apply: (value) => applied.push(value), save: async (value) => {
      if (failSave) throw new Error('disk full');
      return value;
    } });
    const removal = controller.removeColorTheme('ai-night', () => deleting.promise);
    const save = controller.change({ colorTheme: 'orange' });
    deleting.resolve({ id: 'ai-night', deleted: true, ui_preferences: { ...localAppearance, colorTheme: 'blue' } });
    await removal;
    assert.equal(applied[0].colorTheme, 'orange');
    await save;
    assert.equal(controller.current().colorTheme, failSave ? 'blue' : 'orange');
    assert.equal(applied.some((value) => value.colorTheme === 'ai-night'), false);
  }
});

await run('failed deletion preserves the theme and does not block later appearance saves', async () => {
  const controller = createAppearancePersistenceController({ initial: localAppearance, apply: () => {}, save: async (value) => value });
  await assert.rejects(controller.removeColorTheme('ai-night', async () => { throw new Error('read only'); }), /read only/);
  assert.deepEqual(controller.current(), localAppearance);
  await controller.change({ fontSize: 'small' });
  assert.equal(controller.confirmed().colorTheme, 'ai-night');
  assert.equal(controller.confirmed().fontSize, 'small');
  await assert.rejects(controller.removeColorTheme('eva-01', () => assert.fail('must not delete built-in')));
});

await run('cleanup pending remains a successful removal and stale bootstrap cannot restore deleted theme', async () => {
  const controller = createAppearancePersistenceController({ initial: localAppearance, apply: () => {}, save: async (value) => value });
  const result = await controller.removeColorTheme('ai-night', async () => ({ id: 'ai-night', deleted: true, cleanup_pending: true }));
  assert.equal(result.cleanup_pending, true);
  assert.equal(controller.current().colorTheme, 'blue');
  assert.equal(controller.restore(localAppearance), false);
  await controller.change({ theme: 'light', colorTheme: 'ai-night' });
  assert.equal(controller.current().colorTheme, 'blue');
});

await run('deleting an inactive theme does not apply the server snapshot over local appearance', async () => {
  const applied = [];
  const controller = createAppearancePersistenceController({ initial: { ...localAppearance, colorTheme: 'orange' }, apply: (value) => applied.push(value), save: async (value) => value });
  await controller.removeColorTheme('ai-night', async () => ({ id: 'ai-night', deleted: true, ui_preferences: { ...localAppearance, colorTheme: 'orange' } }));
  assert.equal(controller.current().colorTheme, 'orange');
  assert.deepEqual(applied, []);
});
