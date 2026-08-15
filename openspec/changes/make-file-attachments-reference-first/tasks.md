## 1. 共享附件引用

- [x] 1.1 新增 session 层普通文件引用 helper，统一生成 ID、元数据、`source_path`、`snapshot_path`、`read_path` 与按需读取说明
- [x] 1.2 为只有快照和同时具有原始路径的两种附件补充 helper 单元测试

## 2. Provider 序列化

- [x] 2.1 将 OpenAI-compatible 普通文件与误标非图片 part 改为共享引用，并移除文本附件自动读取
- [x] 2.2 将 Anthropic 普通文件 part 改为共享引用，同时保留无效 metadata 的明确降级提示
- [x] 2.3 更新 Provider 回归测试，覆盖正文不内联、路径字段、Provider 一致性与图片行为不变

## 3. 验证

- [x] 3.1 运行附件引用与 Provider 的针对性 C++ 单元测试
- [x] 3.2 运行 OpenSpec 严格验证并复核最终差异不包含 API、持久化或图片行为漂移
