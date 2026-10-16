## Context

现有 ModelSamplingOptions 和 provider 请求构造已支持四种采样字段。两种子 agent 工具均通过 SessionRegistry 创建会话；专家团引用单专家，旧格式也支持包内 Agent。SessionModelBinding 在模型构造后重新解析配置，单改首次构造会丢失覆盖值。当前专家解绑只清元数据，已加载角色状态按现有生命周期保留。

## Goals / Non-Goals

**Goals:** 复用采样值类型、范围校验、协议校验和 provider factory。覆盖值只能修改会话局部 profile 副本，并随 session metadata 保存。

**Non-Goals:** 不新增模型参数种类、推理强度配置、全局默认值或第二套专家执行引擎；不改既有解绑契约。

## Decisions

1. 在 saved_models 辅助层提供字段合并与严格覆盖解析。旧 saved_models 的未知键兼容规则不变，新的工具/专家入口拒绝拼错字段。null 字段代表未覆盖。
2. ExpertDefinition 与包内 ExpertAgent 保存 sampling；所选 Agent 的值覆盖包级默认。引用成员投影其自身有效预设。专家 CRUD 使用 presence 标记，省略保留、null/{} 清除；单专家详情展示有效预设，显式编辑会一并替换其 lead 的包内覆盖，防止隐藏值抵消表单修改。团队编辑仅修改团队默认。编辑器始终明确提交当前值。
3. SessionOptions/SessionMeta/SessionManager 保存 `sampling` 显式覆盖，SessionEntry 保留合并后的会话覆盖快照，所有模型 resolver 均应用快照后验证支持。先解析专家再创建 provider，避免发起时覆盖被 revalidation 丢弃。恢复时使用持久化覆盖和当前专家预设。
4. 专家切换在原控制边界、model_control_mu 下构造并发布对应 provider；元数据写入失败恢复旧 provider。采样字段不直接修改共享 provider，进行中的请求继续持有原快照。
5. 两个 spawn 工具始终暴露 sampling，蜂群已有 model/reasoning 开关只控制既有字段。省略 sampling 使用所选专家/模型默认，不额外复制父会话的显式覆盖；历史 fork 与采样覆盖独立。
6. 专家编辑器沿用现有基本信息区样式，以可选数字输入表示继承。协议校验在执行时按实际选用模型进行；编辑器与保存 API 验证通用范围。
7. 遵守 R12 文件行数上限：专家定义选择方法移入 `expert_definition.cpp`，会话模型元数据方法移入 `session_manager_model.cpp`，共享模型快照和覆盖解析移入 `session_model_resolution.cpp`。保留原接口与锁序，不提高体积基线。

## Risks / Trade-offs

- 专家预设可能无法用于某些模型协议 → 在创建与切换前明确拒绝，不静默丢弃。
- 遗留无可解析模型、仅借用 provider 的嵌入路径无法安全重建 provider → 未配置覆盖保持现状，显式配置则失败说明原因。
- 采样修改与模型热重载并发 → 复用 model_control_mu 与不可变 resolver 捕获，保留已有锁序和快照生命周期。

## Migration Plan

缺少 sampling 的旧专家包、会话与调用无需迁移。回滚程序会忽略新增元数据和专家字段。验证包括专家 CRUD、参数合并/拒绝、两种派生入口、恢复与重载、Web 全量测试/构建及 Windows 定向原生测试。
