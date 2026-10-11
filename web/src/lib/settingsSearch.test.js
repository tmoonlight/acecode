import assert from 'node:assert/strict';
import { settingsSearchEntries, searchSettings, settingsSearchResultIndex } from './settingsSearch.js';
const entries = settingsSearchEntries();
assert.deepEqual(searchSettings(entries, '  '), []);
assert.deepEqual(searchSettings(entries, 'not-a-real-setting'), []);
assert.equal(searchSettings(entries, '终端程序路径')[0].section, 'config');
assert.equal(searchSettings(entries, '终端程序路径')[0].label, '终端程序路径');
assert.equal(searchSettings(entries, 'terminal program path')[0].label, '终端程序路径');
assert.equal(searchSettings(entries, 'NODE.JS')[0].label, 'Node.js 工具');
assert.equal(searchSettings(entries, 'font size')[0].section, 'appearance');
assert.ok(searchSettings(entries, 'path').length > 3);
assert.equal(searchSettings(entries, 'upgrade')[0].section, 'config');
assert.equal(searchSettings(entries, 'Skill 扫描路径')[0].section, 'config');
assert.equal(searchSettings(entries, 'skill scan paths')[0].label, 'Skill 扫描路径');
console.log('[pass] bilingual settings search, ranking, cross-section and empty queries');

// 全局搜索面板 → 设置窗口的选中项对齐(settingsSearchResultIndex)。
// 触发场景:用户在 Ctrl+K 面板里搜 'path',从 8 条设置命中里选了不是第一条的「工作空间路径」。
// 期望:设置窗口用同一个查询重跑后,选中下标指向同一条(按 id 对齐);
// id 对不上(开发者模式解锁状态两边不一致导致 id 错位)时退回 section+label;
// 都对不上或入参缺失时取 0,永远不会越界。
{
  const results = searchSettings(entries, 'path');
  const target = results.find((item) => item.label === '工作空间路径');
  assert.ok(target, 'fixture: 工作空间路径 must be a hit for "path"');
  const expected = results.indexOf(target);
  assert.ok(expected > 0, 'fixture: the target must not already be the first hit');
  assert.equal(settingsSearchResultIndex(results, target), expected);
  assert.equal(settingsSearchResultIndex(results, { id: 'setting-999', section: target.section, label: target.label }), expected);
  assert.equal(settingsSearchResultIndex(results, { id: 'setting-999', section: 'nowhere', label: '不存在' }), 0);
  assert.equal(settingsSearchResultIndex(results, null), 0);
  assert.equal(settingsSearchResultIndex([], target), 0);
  assert.equal(settingsSearchResultIndex(undefined, target), 0);
  console.log('[pass] palette-to-settings result index aligns by id, falls back to label, never overflows');
}
