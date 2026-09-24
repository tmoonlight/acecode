import { getSettingsNavItems } from './settingsNavigation.js';
import { sourceCatalogs } from '../i18n/sourceCatalog.generated.js';

// Labels are also the destination anchors. Both catalog languages remain searchable.
export function settingsSearchEntries(developerModeUnlocked = false) {
  const navItems = getSettingsNavItems(developerModeUnlocked);
  const fields = [
    ['general', '界面语言', 'language locale english chinese'],
    ['general', '工作模式', 'work mode coding daily'],
    ['general', '打开任务完成通知', 'notification completion sound'],
    ['general', '新手指引', 'onboarding getting started tour'],
    ['general', '权限模式', 'permission approval sandbox'],
    ['general', '默认打开目标', 'default open target editor'],
    ['general', '最大轮次', 'max turns limit'],
    ['general', '后台进程状态', 'background daemon process'],
    ['general', '关闭窗口时', 'close window tray exit'],
    ['general', '远程 Web 模式', 'remote web network port bind'],
    ['appearance', '主题', 'theme accent blue orange color'],
    ['appearance', '暗黑模式', 'dark light mode'],
    ['appearance', '字体大小', 'font size'],
    ['appearance', '显示任务时间', 'sidebar task time timestamp'],
    ['appearance', '消息自动折叠', '会话 conversation messages auto collapse fold expand tools'],
    ['config', '升级服务 URL', 'upgrade update service url'],
    ['config', 'Python 工具', 'python uv ruff mypy path directory'],
    ['config', 'Node.js 工具', 'node nodejs npm pnpm tsx path directory'],
    ['config', 'C# 工具', 'csharp dotnet roslyn sdk path directory'],
    ['config', '终端类型', 'default terminal shell powershell pwsh cmd bash zsh fish'],
    ['config', '终端程序路径', 'terminal program executable path powershell pwsh'],
    ['config', '工作空间路径', 'workspace data directory path migrate backup'],
    ['personalization', '自定义指令', 'custom instructions prompt personalization'],
    ['skills', '工作区 Skill 目录', 'workspace skill directory'],
    ['mcp', '服务器配置', 'mcp server config json'],
    ['tools', '内置工具', 'builtin tools'],
    ['tools', 'Agent 浏览器', 'agent browser'],
    ['tools', '电脑操控（实验性）', 'computer use experimental windows desktop mouse keyboard screenshot pointer cursor style theme color ace plain 指针 样式 主题色'],
    ['tools', '图像生成', 'image generation drawing'],
    ['tools', '摘要生成', 'summary title generation local model 摘要模型 会话标题'],
    ['tools', '工具重写', 'tool rewrite rename alias audit'],
    ['security', '沙箱安全', 'sandbox security isolation network access'],
    ['security', '文件安全', 'file security allow deny list path whitelist blacklist'],
    ['security', '命令安全', 'command security prefix rules allow prompt forbidden'],
    ['security', '审计中心', 'audit log history export clear blocked'],
    ['archived', '搜索已归档会话', 'archived sessions restore delete search sort workspace'],
    ['usage', 'Token 活动', 'daily weekly cumulative tokens usage statistics heatmap'],
    ['usage', '模型用量明细', 'model tokens usage'],
    ['usage', '工作区用量', 'workspace usage'],
    ['feedback', '反馈内容', 'feedback message bug'],
    ['about', '当前版本', 'version upgrade'],
    ['about', 'Web 核心', 'webview browser engine'],
  ];
  if (developerModeUnlocked) {
    fields.push(['developer', '允许多进程启动', 'developer multiple desktop instances processes']);
    fields.push(['developer', '打开JB模式', 'jb mode dark heat wave refusal']);
    fields.push(['developer', '工具前言', 'tool preamble progress title status line reasoning summary sidecar 前言 标题']);
  }
  const all = [
    ...fields.map(([section, label, aliases], index) => ({ id: `setting-${index}`, section, label, aliases })),
    ...navItems.map((item) => ({ id: `section-${item.key}`, section: item.key, label: item.label, aliases: item.key })),
  ];
  const catalogs = Object.entries(sourceCatalogs['zh-CN']);
  return all.map((item) => {
    const source = catalogs.find(([key, zh]) => zh === item.label || sourceCatalogs['en-US'][key] === item.label);
    return { ...item, translations: source ? [source[1], sourceCatalogs['en-US'][source[0]]] : [],
      sectionLabel: navItems.find((nav) => nav.key === item.section)?.label || item.section };
  });
}

function normalize(value) {
  return String(value || '').normalize('NFKC').toLocaleLowerCase().replace(/[\s._-]+/gu, ' ').trim();
}

export function searchSettings(entries, query) {
  const needle = normalize(query);
  if (!needle) return [];
  const words = needle.split(/\s+/u);
  return entries.map((item) => {
    const labels = [item.label, ...(item.translations || [])].map(normalize);
    const text = normalize([item.label, item.sectionLabel, item.aliases, ...(item.translations || [])].join(' '));
    const score = labels.some((label) => label === needle) ? 100
      : labels.some((label) => label.startsWith(needle)) ? 80
      : labels.some((label) => label.includes(needle)) ? 60
      : words.every((word) => text.includes(word)) ? 20 : 0;
    return { item, score };
  }).filter((entry) => entry.score).sort((a, b) => b.score - a.score).map((entry) => entry.item);
}

// 全局搜索面板选中某条设置后,设置窗口要把同一条结果选成当前项。
// 先按 id 对齐(两边用同一份 settingsSearchEntries 生成,id 稳定),
// 开发者模式解锁状态不一致时 id 可能错位,退回 section+label 匹配;都找不到取 0。
export function settingsSearchResultIndex(results, target) {
  if (!Array.isArray(results) || results.length === 0 || !target) return 0;
  const byId = target.id ? results.findIndex((item) => item.id === target.id) : -1;
  if (byId >= 0) return byId;
  const byLabel = results.findIndex((item) => item.section === target.section && item.label === target.label);
  return byLabel >= 0 ? byLabel : 0;
}

export function locateSetting(root, result) {
  if (!root || !result) return null;
  const walker = root.ownerDocument.createTreeWalker(root, 4);
  const labels = [result.label, ...(result.translations || [])].map(normalize);
  let node;
  while ((node = walker.nextNode())) {
    const element = node.parentElement;
    if (!element || element.closest('select, option, button, textarea, [hidden]')) continue;
    if (labels.includes(normalize(node.textContent)) && element.getClientRects().length) return element;
  }
  return null;
}
