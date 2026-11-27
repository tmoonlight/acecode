import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const dialog = fs.readFileSync(
  path.resolve(here, '../components/model-settings/ModelProfileDialog.jsx'),
  'utf8',
);
const picker = fs.readFileSync(
  path.resolve(here, '../components/model-settings/ProviderCatalogPicker.jsx'),
  'utf8',
);

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

run('自定义兼容 API 的基础字段集中到 Provider 右侧详情区', () => {
  // custom-openai 与 custom-anthropic 共用同一套右侧详情区布局。
  assert.match(dialog, /const customCompatibilityApi = isCustomCompatibilityProvider\(provider\);/);
  assert.match(
    dialog,
    /directModelDetails=\{customCompatibilityApi \? \([\s\S]*?<CustomCompatibilityApiFields/,
  );
  assert.match(
    picker,
    /\{directModelIdInput \? \([\s\S]*?id="custom-openai-model-id"[\s\S]*?\{directModelDetails\}[\s\S]*?\) : \(/,
  );
  // 别名放在 Base URL / API Key 之上,与非自定义 Provider 的字段顺序一致。
  assert.match(dialog, /function CustomCompatibilityApiFields\([\s\S]*?<ModelAliasField[\s\S]*?Base URL[\s\S]*?API Key/);
});

run('自定义兼容 API 不在 Provider 选择器下方重复基础字段', () => {
  assert.match(dialog, /!policy\?\.managed && !customCompatibilityApi && \(/);
  // 非自定义 Provider 的别名字段只渲染一次(选择器下方、连接字段之前),
  // 高级设置与编辑模式里不再各放一份「预设名称」。
  assert.match(dialog, /\{!customCompatibilityApi && \(\s*<ModelAliasField/);
  assert.equal((dialog.match(/<ModelAliasField/g) || []).length, 2, '自定义一份 + 非自定义一份');
  assert.doesNotMatch(dialog, /预设名称', true\)/);
  assert.doesNotMatch(dialog, /id="model-profile-name"[\s\S]*?id="model-profile-name"[\s\S]*?id="model-profile-name"/);
});

// 「复用已有凭据」下拉已删除:draft 与 payload 都不再携带 credential_source_name,
// 弹窗里也没有对应控件。回归哨兵,防止后续把这块又接回来。
run('新增模型弹窗不再提供复用已有凭据', () => {
  assert.doesNotMatch(dialog, /credential_source_name|compatibleCredentialSources|复用已有凭据/);
  const settings = fs.readFileSync(path.resolve(here, './modelSettings.js'), 'utf8');
  assert.doesNotMatch(settings, /credential_source_name|compatibleCredentialSources/);
});

// 所有会改变模型 / Provider 选择的草稿更新都要经过别名同步,否则自动别名不会跟着刷新;
// 反过来 patchDraft(改别名 / API Key 等其它字段)不能走它,否则会覆盖用户正在手打的别名。
run('模型选择变化统一经 syncAutoModelAlias 刷新自动别名', () => {
  assert.match(dialog, /const withAutoAlias = \(next\) => syncAutoModelAlias\(next, \{/);
  assert.match(dialog, /onDraftChange=\{changeSelectionDraft\}/);
  assert.match(dialog, /withAutoAlias\(applyCatalogProviderToDraft\(current, nextProvider\)\)/);
  assert.match(dialog, /buildModelMutationPayloads\(draft, provider, \{ editing, existingNames \}\)/);
  assert.doesNotMatch(dialog, /onDraftChange=\{setDraft\}/);
});

// Anthropic(官方与自定义兼容网关)和 OpenAI 兼容一样按 Base URL + /models 探测:
// 探测请求必须带上 base_url / api_key,自定义表单 Model ID 旁的「探测模型」按钮也要出现。
// 回归点:之前只对 runtime_provider === 'openai' 打开,Anthropic 只能手填模型 ID。
run('Anthropic 协议同样打开模型探测', () => {
  assert.match(
    picker,
    /function isHttpProbeProvider\(runtimeProvider\) \{\s*return runtimeProvider === 'openai' \|\| runtimeProvider === 'anthropic';/,
  );
  assert.match(picker, /base_url: http \? draft\.base_url : ''/);
  assert.match(picker, /api_key: http \? draft\.api_key : ''/);
  assert.match(picker, /\{httpProbeProvider && \(\s*<button[\s\S]*?onClick=\{openProbeDialog\}/);
  assert.match(picker, /\{!directModelIdInput && supportsProbe && \(/);
  assert.doesNotMatch(picker, /provider\.runtime_provider === 'openai' && \(/);
});
