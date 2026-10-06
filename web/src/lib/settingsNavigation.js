export const SETTINGS_NAV_GROUPS = [
  {
    key: 'personal',
    label: '个人',
    items: [
      { key: 'general', label: '常规', icon: 'settings' },
      { key: 'appearance', label: '外观', icon: 'brightness' },
      { key: 'config', label: '配置', icon: 'terminal' },
      { key: 'personalization', label: '个性化', icon: 'eye' },
      { key: 'usage', label: '使用情况', icon: 'list' },
    ],
  },
  {
    key: 'integrations',
    label: '集成',
    items: [
      { key: 'skills', label: '技能', icon: 'lightbulb' },
      { key: 'mcp', label: 'MCP 服务器', icon: 'mcp' },
      { key: 'connectors', label: '插件', icon: 'extension' },
      { key: 'channels', label: '消息通道', icon: 'chat' },
    ],
  },
  {
    key: 'coding',
    label: '编码',
    items: [
      { key: 'models', label: '模型', icon: 'brain' },
      { key: 'tools', label: '工具', icon: 'tool' },
      { key: 'hooks', label: '钩子', icon: 'hook' },
      { key: 'security', label: '安全中心', icon: 'lock' },
    ],
  },
  {
    key: 'archived',
    label: '已归档',
    items: [
      { key: 'archived', label: '已归档会话', icon: 'archive' },
    ],
  },
  {
    key: 'support',
    label: '支持',
    items: [
      { key: 'feedback', label: '问题反馈', icon: 'help' },
      { key: 'about', label: '关于', icon: 'info' },
    ],
  },
];

export const SETTINGS_NAV_ITEMS =
  SETTINGS_NAV_GROUPS.flatMap((group) => group.items);

const SETTINGS_NAV_INDEX_BY_KEY = new Map(
  SETTINGS_NAV_ITEMS.map((item, index) => [item.key, index]),
);

export function getSettingsNavGroups(developerModeUnlocked = false) {
  if (!developerModeUnlocked) return SETTINGS_NAV_GROUPS;
  return SETTINGS_NAV_GROUPS.map((group) => group.key === 'support'
    ? { ...group, items: [...group.items, { key: 'developer', label: '开发者模式', icon: 'terminal' }] }
    : group);
}

export function getSettingsNavItems(developerModeUnlocked = false) {
  return getSettingsNavGroups(developerModeUnlocked).flatMap((group) => group.items);
}

export function settingsNavIndexForKey(key, developerModeUnlocked = false) {
  if (key === 'developer' && developerModeUnlocked) return SETTINGS_NAV_ITEMS.length;
  return SETTINGS_NAV_INDEX_BY_KEY.get(key) ?? 0;
}
