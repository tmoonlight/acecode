import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { PROVIDER_LOGO_ASSET_BY_ID } from './providerLogos.generated.js';
import { providerLogoAssetId, providerLogoPath } from './providerLogos.js';

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
const providerIcon = source('components/model-settings/ProviderIcon.jsx');
const providerGroups = source('lib/providerCatalogGroups.js');
const saved = source('components/model-settings/SavedModelList.jsx');
const helpers = source('lib/modelSettings.js');
const focusedComponents = [section, dialog, picker, connection, providerIcon, saved].join('\n');

run('model settings navigation delegates to focused list-first components', () => {
  assert.match(
    settingsPage,
    /import \{ ModelSettingsSection \} from '\.\/model-settings\/ModelSettingsSection\.jsx';/,
  );
  assert.match(settingsPage, /return <ModelSettingsSection onModelProfileUpdated=\{onModelProfileUpdated\} \/>;/);
  assert.doesNotMatch(settingsPage, /function draftFromModelProfile/);

  const savedIndex = section.indexOf('<SavedModelList');
  assert.ok(savedIndex >= 0);
  assert.doesNotMatch(section, /<ModelConnectionCard/);
  assert.doesNotMatch(section, /RecommendedModelList|recommendedModels|热门预置/);
});

run('one adaptive profile Modal owns add and edit flows without template mode', () => {
  assert.match(section, /mode: 'add'/);
  assert.match(section, /mode: 'edit'/);
  assert.doesNotMatch(section, /mode: 'template'/);
  assert.equal((section.match(/<ModelProfileDialog/g) || []).length, 1);
  assert.match(dialog, /mode === 'edit'/);
  assert.doesNotMatch(dialog, /mode === 'template'|templateWarning|templateActive/);
  assert.match(dialog, /<ProviderCatalogPicker/);
  assert.match(dialog, /dismissOnBackdrop=\{!submitting\}/);
  assert.match(dialog, /dismissOnEscape=\{!submitting\}/);
});

run('Copilot and Grok remain managed saved profiles without key or endpoint controls', () => {
  assert.match(dialog, /!policy\?\.managed/);
  assert.match(dialog, /policy\?\.show_api_key/);
  assert.match(picker, /runtime_provider === 'copilot'[\s\S]*?runtime_provider === 'grok'/);
  assert.match(picker, /<ModelConnectionCard \{\.\.\.managedConnection\} \/>/);
  assert.match(dialog, /managedConnections\?\.\[provider\?\.runtime_provider\]/);
  assert.match(section, /managedConnections=\{\{/);
  assert.match(section, /grok: \{/);
  assert.match(section, /api\.startGrokAuth\(\)/);
  assert.match(section, /api\.pollGrokAuth\(grokFlow\.device_code\)/);
  assert.match(section, /api\.logoutGrok\(\)/);
  assert.match(connection, /受管 Provider 在这里完成登录/);
  assert.match(connection, /flow\?\.device_code/);
  assert.match(connection, /onCopyCode\?\.\(flow\.user_code\)/);
  assert.match(connection, /onPoll/);
  assert.match(connection, /受管端点/);
  assert.doesNotMatch(connection, /base_url|Base URL|font-mono/);
  assert.match(section, /announceMutation\(added\)/);
});

run('API Key is prefilled behind a password mask with an accessible eye toggle', () => {
  assert.match(helpers, /api_key: model\.api_key/);
  assert.match(dialog, /const \[apiKeyVisible, setApiKeyVisible\] = useState\(false\)/);
  assert.match(dialog, /type=\{apiKeyVisible \? 'text' : 'password'\}/);
  assert.match(dialog, /aria-label=\{apiKeyVisible \? '隐藏 API Key' : '显示 API Key'\}/);
  assert.match(dialog, /aria-pressed=\{apiKeyVisible\}/);
  assert.match(dialog, /aria-controls="model-api-key"/);
  assert.match(dialog, /<VsIcon name="eye" size=\{14\} \/>/);
  assert.doesNotMatch(dialog, /已保存密钥不会读取到浏览器/);
});

run('provider picker keeps catalog queries bounded and supports docs manual ids probe and multi-select', () => {
  assert.match(providerGroups, /custom: '自定义模型'/);
  assert.match(providerGroups, /popular: '热门模型'/);
  assert.match(providerGroups, /'custom',[\s\S]*?'popular',[\s\S]*?'native',[\s\S]*?'local',[\s\S]*?'catalog'/);
  assert.match(picker, /groupCatalogProviders\(providers, providerQuery\)/);
  assert.match(section, /providers\.find\(\(provider\) => provider\.id === 'custom-openai'\)/);
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
  assert.match(picker, /<ProviderIcon provider=\{item\} active=\{active\} \/>/);
  assert.match(picker, /flex h-9 w-full items-center gap-2/);
  assert.match(picker, /focus:ring-1 focus:ring-inset focus:ring-accent/);
  assert.doesNotMatch(picker, /item\.runtime_provider === 'copilot'[\s\S]*?item\.id/);
});

run('Provider logos are local deduplicated assets within the package budget', () => {
  const logoDir = path.resolve(srcRoot, '../public/provider-logos');
  const manifest = JSON.parse(fs.readFileSync(path.join(logoDir, 'MANIFEST.json'), 'utf8'));
  const catalog = JSON.parse(fs.readFileSync(
    path.resolve(srcRoot, '../../assets/models_dev/api.json'),
    'utf8',
  ));
  const svgFiles = new Set(fs.readdirSync(logoDir).filter((name) => name.endsWith('.svg')));
  assert.equal(manifest.provider_count, Object.keys(catalog).length);
  assert.equal(manifest.provider_count, Object.keys(PROVIDER_LOGO_ASSET_BY_ID).length);
  assert.equal(manifest.unique_asset_count, svgFiles.size);
  assert.ok(manifest.bundled_svg_bytes <= manifest.max_bundled_svg_bytes);
  Object.values(PROVIDER_LOGO_ASSET_BY_ID).forEach((assetId) => {
    assert.ok(svgFiles.has(`${assetId}.svg`), `missing logo asset: ${assetId}`);
  });
  assert.equal(providerLogoAssetId({ id: 'copilot' }), PROVIDER_LOGO_ASSET_BY_ID['github-copilot']);
  assert.equal(providerLogoAssetId({ id: 'grok' }), PROVIDER_LOGO_ASSET_BY_ID.xai);
  assert.equal(providerLogoAssetId({ id: 'custom-openai' }), PROVIDER_LOGO_ASSET_BY_ID.openai);
  assert.equal(
    providerLogoAssetId({ provider: 'openai', models_dev_provider_id: 'deepseek' }),
    PROVIDER_LOGO_ASSET_BY_ID.deepseek,
  );
  assert.equal(providerLogoAssetId({ provider: 'openai' }), PROVIDER_LOGO_ASSET_BY_ID.openai);
  assert.equal(
    providerLogoAssetId({ provider: 'copilot' }),
    PROVIDER_LOGO_ASSET_BY_ID['github-copilot'],
  );
  assert.equal(providerLogoPath({ id: 'future-provider' }), '');
  assert.doesNotMatch(providerIcon, /https:\/\/models\.dev/);
  assert.match(saved, /<ProviderIcon provider=\{model\} \/>/);
  assert.doesNotMatch(saved, /<VsIcon name="extension"/);
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
});

run('model settings use semantic theme tokens and reserve mono for request-header JSON', () => {
  assert.doesNotMatch(focusedComponents, /(?:bg|text|border)-gray-/);
  assert.doesNotMatch(focusedComponents, /#[0-9a-fA-F]{3,8}/);
  assert.doesNotMatch(connection, /font-mono/);
  assert.equal((focusedComponents.match(/font-mono/g) || []).length, 1);
  assert.match(dialog, /id="model-request-headers"[\s\S]*?font-mono/);
});

run('rows search controls dialogs and busy states expose keyboard and ARIA contracts', () => {
  assert.match(connection, /aria-labelledby=\{headingId\}/);
  assert.match(connection, /id=\{headingId\}/);
  assert.match(saved, /aria-labelledby="saved-models-title"/);
  assert.match(saved, /type="search"/);
  assert.match(saved, /aria-label=\{`编辑模型 \$\{model\.name\}`\}/);
  assert.match(dialog, /labelledBy="model-profile-dialog-title"/);
  assert.match(dialog, /role="alert"/);
  assert.match(picker, /event\.key === 'Enter'/);
  assert.match(picker, /aria-multiselectable=\{allowMultiple \|\| undefined\}/);
  assert.match(section, /disabled=\{!!mutationBusy \|\| deleteTarget\.blocked\}/);
});
