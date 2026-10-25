// 设置 > 常规 >「压缩图片」的纯逻辑。数据在 daemon 配置 image_compression.enabled
// (默认开启):开启时发给模型的图片按 grok-build 的上限压缩,请求体逼近上限时
// 较早的图片换成文件引用;会话里保存的原图不动。请求由组件发,这里只做映射。

export const DEFAULT_IMAGE_COMPRESSION_ENABLED = true;

// GET / PUT /api/config/image-compression 的响应 → 开关状态。
// 读不到 / 字段不对时按 daemon 的默认值(开启)显示。
export function imageCompressionEnabled(state) {
  return state && typeof state === 'object' && typeof state.enabled === 'boolean'
    ? state.enabled
    : DEFAULT_IMAGE_COMPRESSION_ENABLED;
}

// 切换开关时要发的 PUT body;非布尔返回 null(不发请求)。
export function imageCompressionUpdate(enabled) {
  return typeof enabled === 'boolean' ? { enabled } : null;
}
