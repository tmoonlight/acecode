import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const srcRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');

function source(relativePath) {
  return fs.readFileSync(path.join(srcRoot, relativePath), 'utf8');
}

function run(name, fn) {
  try {
    fn();
    console.log(`[pass] ${name}`);
  } catch (error) {
    console.error(`[fail] ${name}`);
    throw error;
  }
}

const settingsPage = source('components/SettingsPage.jsx');
const section = source('components/model-settings/ModelSettingsSection.jsx');
const dialog = source('components/model-settings/ModelProfileDialog.jsx');
const picker = source('components/model-settings/ProviderCatalogPicker.jsx');
const connection = source('components/model-settings/ModelConnectionCard.jsx');
const recommended = source('components/model-settings/RecommendedModelList.jsx');
const saved = source('components/model-settings/SavedModelList.jsx');
const helpers = source('lib/modelSettings.js');
const focusedComponents = [section, dialog, picker, connection, recommended, saved].join('\n');

run('model settings navigation delegates to focused list-first components', () => {
  assert.match(
    settingsPage,
    /import \{ ModelSettingsSection \} from '\.\/model-settings\/ModelSettingsSection\.jsx';/,
  );
  assert.match(settingsPage, /return <ModelSettingsSection onModelProfileUpdated=\{onModelProfileUpdated\} \/>;/);
  assert.doesNotMatch(settingsPage, /function draftFromModelProfile/);

  const connectionIndex = section.indexOf('<ModelConnectionCard');
  const recommendedIndex = section.indexOf('<RecommendedModelList');
  const savedIndex = section.indexOf('<SavedModelList');
  assert.ok(connectionIndex >= 0);
  assert.ok(recommendedIndex > connectionIndex);
  assert.ok(savedIndex > recommendedIndex);
});

run('one adaptive profile Modal owns add edit and template flows', () => {
  assert.match(section, /mode: 'add'/);
  assert.match(section, /mode: 'template'/);
  assert.match(section, /mode: 'edit'/);
  assert.equal((section.match(/<ModelProfileDialog/g) || []).length, 1);
  assert.match(dialog, /mode === 'edit'/);
  assert.match(dialog, /mode === 'template'/);
  assert.match(dialog, /<ProviderCatalogPicker/);
  assert.match(dialog, /dismissOnBackdrop=\{!submitting\}/);
  assert.match(dialog, /dismissOnEscape=\{!submitting\}/);
});

run('Copilot remains a managed saved profile without key or endpoint controls', () => {
  assert.match(dialog, /provider\?\.runtime_provider === 'copilot' \? \(/);
  assert.match(dialog, /Copilot 始终使用 ACECode 管理的认证和服务端点/);
  assert.match(dialog, /\) : \(\s*<div className="grid grid-cols-1 gap-3 md:grid-cols-2">[\s\S]*?policy\?\.show_base_url/);
  assert.match(dialog, /policy\?\.show_api_key/);
  assert.match(connection, /密钥和端点不会进入模型弹窗/);
  assert.match(connection, /受管端点/);
  assert.doesNotMatch(connection, /base_url|Base URL|font-mono/);
  assert.match(section, /announceMutation\(added\)/);
});

run('provider picker keeps catalog queries bounded and supports docs manual ids probe and multi-select', () => {
  assert.match(picker, /local: '本地服务'/);
  assert.match(picker, /\['native', 'local', 'catalog', 'custom'\]/);
  assert.match(picker, /queryModelCatalog\(provider\.id, modelQuery, 50\)/);
  assert.match(picker, /queryModelCatalog\(provider\.id, currentId, 1\)/);
  assert.match(picker, /modelMetadataSummary\(model\)/);
  assert.match(picker, /formatModelTokenLimit/);
  assert.match(picker, /provider\.doc/);
  assert.match(picker, /probeModels/);
  assert.match(picker, /allowMultiple/);
  assert.match(picker, /输入模型 ID，按 Enter 添加/);
  assert.match(picker, /role="listbox"/);
  assert.match(picker, /aria-selected=\{active\}/);
  assert.match(picker, /aria-selected=\{selected\}/);
});

run('templates are API-driven drafts and expose catalog and privacy warnings', () => {
  assert.match(section, /const recommendedModels = catalog\?\.recommended_models \|\| \[\]/);
  assert.match(section, /modelProfileDraftFromTemplate\(template, provider\)/);
  assert.match(section, /warning: template\.privacy_warning/);
  assert.match(recommended, /items\.map\(\(item\) =>/);
  assert.match(recommended, /item\.privacy_warning/);
  assert.match(recommended, /!loading && items\.length > 0/);
  assert.doesNotMatch(recommended, /xiaomi\/mimo|deepseek-v4|nemotron-3/);
  assert.match(dialog, /const templateActive = mode === 'template' && isTemplateDraftActive/);
  assert.match(dialog, /templateActive && templateWarning/);
  assert.match(dialog, /allowMultiple=\{!editing && !templateActive\}/);
});

run('runtime mutation payloads compact reasoning, clear edits, and keep batch metadata per model', () => {
  assert.match(helpers, /export function serializeModelReasoningMutation/);
  assert.match(helpers, /else if \(options\.editing\) payload\.context_window = null/);
  assert.match(helpers, /payload\.request_headers = \{\}/);
  assert.match(helpers, /_catalog_model_metadata/);
  assert.match(helpers, /draftForSelectedModelMutation/);
  assert.match(picker, /toggleCatalogModelInDraft/);
  assert.match(picker, /addManualModelToDraft/);
  assert.match(dialog, /markModelMetadataOverrides/);
  assert.match(dialog, /toggleModelCapability/);
  assert.doesNotMatch(dialog, /next\.has\(capability\).*next\.delete/s);
  assert.match(helpers, /normalizedCapability === 'reasoning'/);
});

run('delete and name conflicts use shared guarded Modals with explicit choices', () => {
  assert.match(section, /<Modal[\s\S]*?labelledBy="delete-model-title"/);
  assert.match(section, /deleteTarget\.name === defaultName/);
  assert.match(section, /deleteTarget\.blocked/);
  assert.match(section, /error\?\.code === 'MODEL_IN_USE'/);
  assert.doesNotMatch(focusedComponents, /window\.confirm/);
  assert.match(dialog, /覆盖已有预设/);
  assert.match(dialog, /另存为/);
  assert.match(dialog, /setConflict\(null\)/);
  assert.match(dialog, /saveAsRef\.current\?\.focus/);
});

run('successful mutations use the existing model profile revision bridge only', () => {
  assert.match(section, /onModelProfileUpdated\?\.\(safe\)/);
  assert.match(section, /onModelProfileUpdated\?\.\(\{ type: 'default'/);
  assert.match(section, /onModelProfileUpdated\?\.\(\{ type: 'delete'/);
  assert.doesNotMatch(dialog, /setDefaultModel|switchModel/);
  assert.doesNotMatch(recommended, /addModel|setDefaultModel|switchModel/);
});

run('model settings use semantic theme tokens and reserve mono for request-header JSON', () => {
  assert.doesNotMatch(focusedComponents, /(?:bg|text|border)-gray-/);
  assert.doesNotMatch(focusedComponents, /#[0-9a-fA-F]{3,8}/);
  assert.doesNotMatch(connection, /font-mono/);
  assert.equal((focusedComponents.match(/font-mono/g) || []).length, 1);
  assert.match(dialog, /id="model-request-headers"[\s\S]*?font-mono/);
});

run('rows search controls dialogs and busy states expose keyboard and ARIA contracts', () => {
  assert.match(connection, /aria-labelledby="model-connections-title"/);
  assert.match(recommended, /aria-labelledby="recommended-models-title"/);
  assert.match(saved, /aria-labelledby="saved-models-title"/);
  assert.match(saved, /type="search"/);
  assert.match(saved, /aria-label=\{`编辑模型 \$\{model\.name\}`\}/);
  assert.match(dialog, /labelledBy="model-profile-dialog-title"/);
  assert.match(dialog, /role="alert"/);
  assert.match(picker, /event\.key === 'Enter'/);
  assert.match(picker, /aria-multiselectable=\{allowMultiple \|\| undefined\}/);
  assert.match(section, /disabled=\{!!mutationBusy \|\| deleteTarget\.blocked\}/);
});
