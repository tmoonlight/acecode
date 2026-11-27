import assert from 'node:assert/strict';
import { autoReviewModelOptions, autoReviewSettingsStore } from './autoReviewSettings.js';

// 自动模式（试验）的裁决模型设置:选项列表与「选了就存」的状态机。

const snapshot = { model_name: '', configured: false,
  models: [{ name: 'main', provider: 'openai', model: 'large' },
    { name: 'reviewer', provider: 'openai', model: 'small' }] };
const copy = (value) => structuredClone(value);
const tick = () => new Promise((resolve) => setImmediate(resolve));

// 场景:下拉的选项。期望:第一项固定是「跟随当前模型」(空值),之后是已保存模型。
assert.deepEqual(autoReviewModelOptions(snapshot, ''), [
  { value: '', label: '跟随当前模型', disabled: false },
  { value: 'main', label: 'main', disabled: false },
  { value: 'reviewer', label: 'reviewer', disabled: false },
]);
// 场景:配置的模型已被删除。期望:补一条禁用项显示旧名字,用户看得到当前存的是什么,
// 但不能再选它。
assert.deepEqual(autoReviewModelOptions(snapshot, 'deleted')[1],
  { value: 'deleted', label: 'deleted', disabled: true });
// 还没加载到数据时只有第一项,不抛错。
assert.deepEqual(autoReviewModelOptions(null, ''), [{ value: '', label: '跟随当前模型', disabled: false }]);

// 场景:加载后选一个模型。期望:立即发出 PUT,只带 model_name;成功后状态与服务端一致。
let saved = copy(snapshot);
const writes = [];
const finishes = [];
const client = {
  getAutoReview: async () => copy(saved),
  setAutoReview: (patch) => new Promise((resolve) => {
    writes.push(copy(patch));
    finishes.push(() => {
      saved = { ...saved, model_name: patch.model_name, configured: !!patch.model_name };
      resolve(copy(saved));
    });
  }),
};
const store = autoReviewSettingsStore(client);
assert.equal(await store.choose('reviewer'), false, 'nothing is saved before the first load');
await store.load();
assert.equal(store.getSnapshot().modelName, '');
const first = store.choose('reviewer');
await tick();
assert.deepEqual(writes, [{ model_name: 'reviewer' }]);
assert.equal(store.getSnapshot().modelName, 'reviewer', 'the choice shows immediately');
assert.equal(store.getSnapshot().saving, true);

// 场景:第一次保存还没返回,用户又改了两次。期望:保存串行进行,中间值不发,
// 只把最后一次的值发出去。
store.choose('main');
store.choose('');
assert.equal(writes.length, 1, 'writes are serialized');
finishes.shift()();
await tick();
assert.deepEqual(writes[1], { model_name: '' });
finishes.shift()();
assert.equal(await first, true);
assert.equal(store.getSnapshot().modelName, '');
assert.equal(store.getSnapshot().saving, false);
assert.equal(saved.model_name, '');

// 场景:同一个 daemon 连接再次打开设置页。期望:拿到同一份状态,不重复创建。
assert.equal(autoReviewSettingsStore(client), store);

// 场景:保存失败(例如模型刚被删掉,服务端返回 400)。期望:选择退回服务端已保存的
// 值并带上错误;之后再选一次可以成功。
let fail = true;
const flaky = autoReviewSettingsStore({
  getAutoReview: async () => copy(snapshot),
  setAutoReview: async (patch) => {
    if (fail) throw Object.assign(new Error('bad'), { status: 400, code: 'BAD_REQUEST' });
    return { ...copy(snapshot), model_name: patch.model_name, configured: true };
  },
});
await flaky.load();
assert.equal(await flaky.choose('reviewer'), false);
assert.equal(flaky.getSnapshot().modelName, '');
assert.deepEqual(flaky.getSnapshot().error, { code: 'BAD_REQUEST', status: 400, action: 'save' });
fail = false;
assert.equal(await flaky.choose('reviewer'), true);
assert.equal(flaky.getSnapshot().modelName, 'reviewer');
assert.equal(flaky.getSnapshot().error, null);

// 场景:后台是旧版本,没有这个接口。期望:错误码归成「不支持」,界面据此提示更新。
const old = autoReviewSettingsStore({
  getAutoReview: async () => { throw Object.assign(new Error('nf'), { status: 404 }); },
  setAutoReview: async () => ({}),
});
await old.load();
assert.equal(old.getSnapshot().error.code, 'AUTO_REVIEW_SETTINGS_UNSUPPORTED');
assert.equal(old.getSnapshot().error.action, 'load');
assert.equal(old.getSnapshot().loading, false);

console.log('[pass] autoReviewSettings');
