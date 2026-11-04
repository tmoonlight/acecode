// 设置 > 常规 >「可视化图表」的纯逻辑(openspec add-inline-visuals)。数据在 daemon 配置
// inline_visuals.enabled(默认开启):开启时 Web / Desktop 会话的系统提示告诉模型可以写
// ```visual 代码块;关闭只影响模型被告知的内容,历史里已有的 visual 照常显示。
// 请求由组件发,这里只做映射。

export const DEFAULT_INLINE_VISUALS_ENABLED = true;

// GET / PUT /api/config/inline-visuals 的响应 → 开关状态。
// 读不到 / 字段不对时按 daemon 的默认值(开启)显示。
export function inlineVisualsEnabled(state) {
  return state && typeof state === 'object' && typeof state.enabled === 'boolean'
    ? state.enabled
    : DEFAULT_INLINE_VISUALS_ENABLED;
}

// 切换开关时要发的 PUT body;非布尔返回 null(不发请求)。
export function inlineVisualsUpdate(enabled) {
  return typeof enabled === 'boolean' ? { enabled } : null;
}
