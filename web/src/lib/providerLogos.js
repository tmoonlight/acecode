import {
  PROVIDER_LOGO_ASSET_BY_ID,
  RASTER_PROVIDER_LOGO_ASSET_IDS,
} from './providerLogos.generated.js';

const PROVIDER_LOGO_ID_OVERRIDES = Object.freeze({
  copilot: 'github-copilot',
  grok: 'xai',
  'custom-openai': 'openai',
});

const FIRST_PARTY_LOGO_PATHS = Object.freeze({
  acemodel: '/acemodel.svg',
});

function firstPartyLogoId(provider) {
  const providerId = String(provider?.id || '').trim();
  const catalogId = String(provider?.models_dev_provider_id || '').trim();
  if (FIRST_PARTY_LOGO_PATHS[providerId]) return providerId;
  if (FIRST_PARTY_LOGO_PATHS[catalogId]) return catalogId;
  return '';
}

export function providerLogoAssetId(provider) {
  const firstPartyId = firstPartyLogoId(provider);
  if (firstPartyId) return firstPartyId;
  const providerId = String(provider?.id || provider?.provider || '').trim();
  const requestedId = PROVIDER_LOGO_ID_OVERRIDES[providerId]
    || String(provider?.models_dev_provider_id || providerId).trim();
  return PROVIDER_LOGO_ASSET_BY_ID[requestedId] || '';
}

export function providerLogoPath(provider) {
  const firstPartyId = firstPartyLogoId(provider);
  if (firstPartyId) return FIRST_PARTY_LOGO_PATHS[firstPartyId];
  const assetId = providerLogoAssetId(provider);
  return assetId ? `/provider-logos/${encodeURIComponent(assetId)}.svg` : '';
}

export function providerLogoIsRaster(provider) {
  return RASTER_PROVIDER_LOGO_ASSET_IDS.has(providerLogoAssetId(provider));
}
