// 读取当前会话可用技能清单的 loader。值为 () => Promise<Array<{name, path, ...}>>。
//
// 技能预览框的「打开文件夹」按钮要按技能名找到 SKILL.md 在哪。项目级技能随工作区不同,
// 所以由 ChatView(主会话)与 SubagentPanel(子会话)按各自的 api 和工作区提供;
// 默认值用当前页面连接、daemon 兼容工作区,供没有 Provider 的渲染路径兜底。
import { createContext } from 'react';
import { createApi } from '../lib/api.js';

let defaultApi = null;

export function defaultSkillCatalogLoader() {
  if (!defaultApi) defaultApi = createApi(null);
  return defaultApi.listSkills('');
}

export const SkillCatalogContext = createContext(defaultSkillCatalogLoader);
