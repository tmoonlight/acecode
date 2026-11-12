// 子代理几何头像的单测:同一个身份总是同一朵、参数落在允许的范围内、花瓣怎么排。
import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  GEOMETRIC_AVATAR_COUNTS,
  GEOMETRIC_AVATAR_FORMS,
  GEOMETRIC_AVATAR_HUES,
  geometricAvatarCenterTone,
  geometricAvatarPetals,
  geometricAvatarSpec,
} from './geometricAvatar.js';

// 场景:同一个子代理的头像出现在对话、摘要面板、后台任务页签,刷新页面后还要是同一朵。
// 期望:同一个身份每次算出完全相同的参数;参数都来自那几张表(5 种花瓣 / 5 种瓣数 / 8 个颜色),
// 深浅两档颜色和花瓣路径与表里一致。
test('同一个身份总是同一朵,参数都来自固定的几张表', () => {
  const first = geometricAvatarSpec('subagent:20261111_162252_a84f');
  assert.deepEqual(geometricAvatarSpec('subagent:20261111_162252_a84f'), first);
  for (let index = 0; index < 500; index += 1) {
    const spec = geometricAvatarSpec(`subagent:sample-${index}`);
    const form = GEOMETRIC_AVATAR_FORMS.find((item) => item.key === spec.form);
    const hue = GEOMETRIC_AVATAR_HUES.find((item) => item.key === spec.hue);
    assert.ok(form, `sample ${index} form`);
    assert.ok(hue, `sample ${index} hue`);
    assert.equal(spec.path, form.path);
    assert.equal(spec.main, hue.main);
    assert.equal(spec.soft, hue.soft);
    assert.ok(GEOMETRIC_AVATAR_COUNTS.includes(spec.count), `sample ${index} count`);
    assert.equal(typeof spec.center, 'boolean');
    assert.equal(typeof spec.turned, 'boolean');
  }
  // 没有身份(空串)也能算出一朵,不抛错。
  assert.ok(geometricAvatarSpec('').form);
});

// 场景:一个会话里派出十几个子代理,id 往往只差末尾几个字符(同一秒建的)。
// 期望:只差一个字符的 id 也散到不同的花上——五个参数各自都能取到全部取值,
// 而且分布大致均匀(每个取值都不少于平均数的 7 成)。
// 回归防护:直接拿哈希的低位取模时,相邻 id 会只在一个参数上变化,看起来像同一朵换了个颜色。
// 阈值 7 成:4000 个样本、最多 8 个取值时,均匀分布下偏差远小于 3 成,低于它说明哈希没搅开。
test('相邻的 id 散到不同的花上,各参数取值均匀', () => {
  const tally = { form: new Map(), count: new Map(), hue: new Map(), center: new Map(), turned: new Map() };
  const seen = new Set();
  const total = 4000;
  for (let index = 0; index < total; index += 1) {
    const spec = geometricAvatarSpec(`subagent:20261111_162252_${index.toString(16).padStart(4, '0')}`);
    seen.add([spec.form, spec.count, spec.hue, spec.center, spec.turned].join('|'));
    for (const [key, counts] of Object.entries(tally)) counts.set(spec[key], (counts.get(spec[key]) || 0) + 1);
  }
  const sizes = { form: 5, count: 5, hue: 8, center: 2, turned: 2 };
  for (const [key, counts] of Object.entries(tally)) {
    assert.equal(counts.size, sizes[key], key);
    for (const [value, hits] of counts) {
      assert.ok(hits >= (total / sizes[key]) * 0.7, `${key}=${value} 只出现 ${hits} 次`);
    }
  }
  // 800 种组合里 4000 个样本应该几乎全部出现过。
  assert.ok(seen.size > 760, `只出现了 ${seen.size} 种组合`);
  const a = geometricAvatarSpec('subagent:20261111_162252_a84f');
  const b = geometricAvatarSpec('subagent:20261111_162252_a84e');
  assert.notDeepEqual(a, b);
});

// 场景:把一片花瓣绕中心转一圈拼成一朵。
// 期望:花瓣个数等于瓣数,角度均匀分布;偶数瓣深浅交替,奇数瓣交替不成一圈所以全用深色;
// 「转半格」时整朵多转半个间隔(四根辐条从「+」变成「×」);花心用和花瓣不同的那一档。
test('花瓣均匀排一圈,偶数瓣深浅交替,奇数瓣全深色', () => {
  const six = geometricAvatarPetals({ count: 6, turned: false });
  assert.deepEqual(six.map((petal) => petal.rotate), [0, 60, 120, 180, 240, 300]);
  assert.deepEqual(six.map((petal) => petal.tone), ['main', 'soft', 'main', 'soft', 'main', 'soft']);

  const five = geometricAvatarPetals({ count: 5, turned: false });
  assert.deepEqual(five.map((petal) => petal.rotate), [0, 72, 144, 216, 288]);
  assert.ok(five.every((petal) => petal.tone === 'main'));

  assert.deepEqual(geometricAvatarPetals({ count: 4, turned: true }).map((petal) => petal.rotate), [45, 135, 225, 315]);
  assert.deepEqual(geometricAvatarPetals({ count: 3, turned: true }).map((petal) => petal.rotate), [60, 180, 300]);
  // 角度互不相同:组件拿它当 React 的 key。
  for (const count of GEOMETRIC_AVATAR_COUNTS) {
    const rotations = geometricAvatarPetals({ count, turned: true }).map((petal) => petal.rotate);
    assert.equal(new Set(rotations).size, count);
  }

  assert.equal(geometricAvatarCenterTone({ count: 6 }), 'main');
  assert.equal(geometricAvatarCenterTone({ count: 5 }), 'soft');
  // 参数缺失时不抛错,至少画一片。
  assert.equal(geometricAvatarPetals(null).length, 1);
});

// 场景:头像只有 16px,颜色是区分子代理的第一要素。
// 期望:8 个颜色互不相同,深浅两档也不相同;没有红色和灰色——红色在界面里表示出错,
// 灰色留给「已中止」的头像。
test('颜色表:八个互不相同的颜色,不占用红色和灰色', () => {
  assert.equal(GEOMETRIC_AVATAR_HUES.length, 8);
  assert.equal(new Set(GEOMETRIC_AVATAR_HUES.map((hue) => hue.key)).size, 8);
  assert.equal(new Set(GEOMETRIC_AVATAR_HUES.map((hue) => hue.main)).size, 8);
  for (const hue of GEOMETRIC_AVATAR_HUES) {
    assert.match(hue.main, /^#[0-9A-F]{6}$/);
    assert.match(hue.soft, /^#[0-9A-F]{6}$/);
    assert.notEqual(hue.main, hue.soft);
    assert.ok(!['red', 'gray', 'grey'].includes(hue.key), hue.key);
  }
});
