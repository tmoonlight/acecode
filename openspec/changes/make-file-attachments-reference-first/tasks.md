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

## 4. Desktop 来源引用入口

- [x] 4.1 让 Desktop 原生 materializer 对普通文件只返回路径、MIME、实际大小和 `reference_only`，并保留图片字节读取与 25 MiB 限制
- [x] 4.2 扩展前端原生文件解析和 composer file 元数据，让来源引用在读取、压缩和 Base64 转换前走独立创建请求
- [x] 4.3 扩展附件创建路由与 `AttachmentStore`，服务端重新校验来源路径和文件类型，并持久化无 blob 的来源引用记录
- [x] 4.4 补齐来源引用提示语、加载兼容和 blob 不可用行为，确保消息发送、恢复与 Provider 序列化继续工作

## 5. 增量回归验证

- [x] 5.1 增加 Desktop materializer、附件存储、HTTP、前端和 Provider 测试，覆盖超过 25 MiB 的 PDF、小普通文件不复制、图片边界及无效路径
- [x] 5.2 运行前端全量测试与构建、相关 C++ 测试、Desktop Release 构建、OpenSpec 严格校验和最终差异审计
