import { enUS } from './catalogs/en-US.js';
import { zhCN } from './catalogs/zh-CN.js';
import { sourceCatalogs } from './sourceCatalog.generated.js';
import { systemNoticeEnUS, systemNoticeZhCN } from './catalogs/systemNotice.js';
import { toolTranscriptEnUS, toolTranscriptZhCN } from './catalogs/toolTranscript.js';

export const translationCatalogs = Object.freeze({
  'zh-CN': { ...zhCN, toolTranscript: toolTranscriptZhCN, systemNotice: systemNoticeZhCN, source: sourceCatalogs['zh-CN'] },
  'en-US': { ...enUS, toolTranscript: toolTranscriptEnUS, systemNotice: systemNoticeEnUS, source: sourceCatalogs['en-US'] },
});
