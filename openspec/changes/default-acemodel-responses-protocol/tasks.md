## 1. 默认协议与运行时

- [x] 1.1 实现共享 ACEModel 协议默认值、旧配置加载与模型列表/目录 API；通过配置及目录契约回归测试验证缺省、明确覆盖及其他 Provider 行为。
- [x] 1.2 Provider 工厂使用有效默认协议；通过本地 HTTP 测试验证 Responses 的流式/非流式请求、连接检测协议和明确切回 Chat Completions。

## 2. 设置与安装器

- [x] 2.1 前端采用目录默认协议并复用 API 协议选择框；通过草稿、批量保存、编辑、切换 Provider 的 JS 回归及界面检查验证。
- [x] 2.2 Windows seeder 写入缺省 Responses 并保留明确选择；运行 seed_acemodel.test.ps1 验证新建、升级与无关配置保留。

## 3. 集成验证

- [x] 3.1 更新 Daemon API 文档并完成定向 C++ 测试、pnpm test、pnpm build、严格 OpenSpec 校验及 git diff --check；记录验证结果及运行环境边界。
