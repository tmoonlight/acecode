## Context

见 proposal.md。当前 ProviderCatalogPicker 仅把探测结果应用到编辑草稿，刷新按钮调用本地 GET /api/models；聊天控件依赖保存的 supported_efforts。配置已有 mutate_config 事务与 saved-model revision，会话具备安全重载机制。

## Goals / Non-Goals

**Goals:** 三个入口复用有生命周期的后台同步器，连接去重、可取消、静默失败并保证并发写入一致性。

**Non-Goals:** 不推测模型名称对应的档位，不替换 models.dev 目录刷新，不新增自动重试或定时轮询，不为不支持现有 OpenAI 兼容探测协议的受管 Provider 构造新协议。

## Decisions

1. Web 服务持有单一工作线程，启动、保存、认证的 POST /api/models/reasoning/refresh 只排队，立即返回。相同连接以端点、凭据、请求头和目录身份去重；并发请求合并，后续保存可排下一轮。使用网络超时与取消回调，退出时 join，不使用捕获服务对象的 detached 线程。
2. 复用现有模型声明解析和请求头/代理逻辑。仅接受有效且非空的 supported_efforts，缺失或无效保留本地声明。OpenAI 兼容完整聊天 URL 不推导 /models；保留原配置。受管 Provider 和 Anthropic 不使用 OpenAI 探测协议。
3. 请求前复制模型快照，IO 期间不持有配置锁。返回后在配置锁和 mutate_config 事务内重读最新磁盘配置，逐项结构比较快照，仅合并未变化项的 reasoning 与 capability 标记。保留 enabled、仍有效的 effort 和兼容的预算；其他字段不变。
4. 写入成功后发布模型 revision，复用会话重载机制刷新空闲会话。WebSocket 发出不含密钥的 model_profiles_updated 事件，模型设置静默重读列表，App 更新现有 modelProfileRevision，使聊天输入框刷新。
5. 保持当前按钮布局，采用既有 ACECode 前端样式。自动同步不显示进度或失败 toast；手动刷新也在后台执行，不清空列表。
6. 新对话已有思考深度控件和创建会话参数传递；缺口是模型列表可能缺少声明，而已有会话仍保留自己的声明快照。进入新对话时独立调度一次后台同步，effect 只依赖 API 及会话身份，不依赖模型版本通知，避免通知反复触发远端请求。输入框刷新按钮同时调度同步并立即重读本地列表；同步成功仍复用既有通知刷新控件，网络失败不阻塞模型显示、输入或发送。选择的档位仅传给新会话，不修改模型默认档位。

## Risks / Trade-offs

- 远端列表不完整 → 缺失声明不撤销本地能力，只有有效新声明参与同步。
- 并发编辑覆盖 → 对磁盘最新配置逐项检查请求快照，变化项跳过，由保存入口的新请求补齐。
- 多个连接串行慢请求 → 每次请求有超时且可取消；同一连接只请求一次，不阻塞界面。
- 已在生成的会话 → 遵守现有会话控制门限，配置版本更新于安全边界应用。

## Migration Plan

无需配置格式迁移。下次服务启动或点击刷新即可补齐旧模型档位；回滚代码后已写入的标准 reasoning 字段仍可被旧版本读取。
