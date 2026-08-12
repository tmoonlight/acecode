import {
  PROVIDER_LOGO_ASSET_BY_ID,
  RASTER_PROVIDER_LOGO_ASSET_IDS,
} from './providerLogos.generated.js';

const PROVIDER_LOGO_ID_OVERRIDES = Object.freeze({
  copilot: 'github-copilot',
  'custom-openai': 'openai',
});

export function providerLogoAssetId(provider) {
  const providerId = String(provider?.id || '').trim();
  const requestedId = PROVIDER_LOGO_ID_OVERRIDES[providerId]
    || String(provider?.models_dev_provider_id || providerId).trim();
  return PROVIDER_LOGO_ASSET_BY_ID[requestedId] || '';
}

export function providerLogoPath(provider) {
  const assetId = providerLogoAssetId(provider);
  return assetId ? `/provider-logos/${encodeURIComponent(assetId)}.svg` : '';
}

export function providerLogoIsRaster(provider) {
  return RASTER_PROVIDER_LOGO_ASSET_IDS.has(providerLogoAssetId(provider));
}
