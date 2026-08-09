## Context

`ExpertComponentsPage` 目前把“新建专家”直接绑定到 `ExpertEditor`。项目已经具备另一条可复用链路：`App` 可以把 `initialDraftText` 作为一次性导航载荷带到真实的新任务 composer，`ChatView` 会让该载荷优先于当前工作区 home draft、将它写回对应工作区草稿并消费导航字段；只有用户之后实际发送以 `/expert-manager` 开头的消息时，daemon 才会按现有 Skill 命令机制激活 `expert-manager`。

用户已经明确选择“预置但不发送”：点击入口后应看到真实的新任务输入框及 `/expert-manager `，但此动作不得创建首轮用户消息、不得自动触发 Agent，也不得在专家管理页复制一个聊天框。现有高级编辑器仍是精确配置专家的受支持入口。

## Goals / Non-Goals

**Goals:**

- 把“新建专家”改成主操作与下拉箭头职责分离的分段按钮，并保持现有主按钮的 32px 高度、内容和视觉优先级。
- 主操作导航到当前工作区的真实新任务 composer，预置恰好为 `/expert-manager ` 的未发送草稿并把焦点交给输入框。
- 下拉菜单只提供“高级模式”，该选项继续打开现有 Agent 型 `ExpertEditor`。
- 复用现有 home draft、工作区选择、一次性导航载荷和 Skill 发送链路，不引入新的会话/API 状态。
- 提供可访问的菜单语义、Escape/外部点击关闭与焦点恢复行为。

**Non-Goals:**

- 不自动发送 `/expert-manager`，不预先创建后端 session，也不自动生成第一条 assistant 回复。
- 不新增“通用对话创建”按钮、专家页内 composer、模拟聊天或第二套草稿存储。
- 不重做 `ExpertEditor`、专家团创建入口、专家包 schema 或 `expert-manager` Skill 内容。
- 不改变用户全局 Skill 启停策略；本变更依赖随 Seed 提供并可被当前工作区发现的 `expert-manager`。

## Decisions

### 1. `App` 负责构造真实新任务导航，专家页只表达用户意图

`ExpertComponentsPage` 新增一个语义明确的 callback，例如 `onStartConversationalCreation`。主按钮只调用该 callback；`App` 使用当前专家页对应的 workspace/ref 经过既有 `homeRefFromWorkspace` 归一化，刷新工作区 Git 信息，并导航到带有 `initialDraftText: '/expert-manager '` 的 home ref。

这沿用 `dispatchExpertToNewTask` 已验证的页面到 composer 交接边界，但不会附加 `expertId`。直接让专家页创建 session 或调用消息 API 会绕过 home composer 的延迟建会话约定，并会产生用户尚未发送内容的空会话，因此不采用。

### 2. 预置内容使用现有一次性草稿载荷，不扩展 session schema

`/expert-manager ` 作为一个集中定义的语义常量传入 `initialDraftText`。末尾空格必须保留，使现有富文本 composer 能把首段识别为完整 Skill token，并把光标放在用户可继续补充需求的位置。

`ChatView` 继续使用既有载荷消费逻辑：该显式导航草稿覆盖同一工作区当前 home draft、只更新该工作区的 draft scope，并在应用后移除 `initialDraftText`，防止父组件重渲染重复覆盖用户刚输入的内容。直到用户手动提交，不能调用 `createSession`、`sendInput` 或 builtin 执行路径。

新增专用 route 字段或另一套“附加 Skill”状态会与现有 slash-Skill 命令形成双重真相，因此不采用。

### 3. 使用一个视觉连续、行为分离的分段按钮

现有主按钮的图标、文案、32px 高度、accent 配色和主点击区域保持不变；右侧追加同高度的窄箭头 segment，并用现有 token 和分隔线表现为一个连续控件。主 segment 只开始对话式创建，箭头 segment 只切换菜单，两者不得互相触发。

箭头提供 `aria-haspopup="menu"`、`aria-expanded` 和可识别的 label。菜单使用 `role="menu"`，唯一选项“高级模式”使用 `role="menuitem"`。菜单在选择、Escape、页面外点击及页面卸载时关闭；Escape 关闭后焦点回到箭头。菜单继续使用 `data-ace-native-overlay="overlap"`，避免 Desktop 原生 Browser surface 遮挡。

把“高级模式”并入主按钮的普通点击菜单会增加默认创建步骤，和用户指定的主操作语义相反，因此不采用。

### 4. 高级模式复用原编辑器状态，不复制创建逻辑

选择“高级模式”后关闭菜单并执行当前的 `setEditor({ editing: false, form: emptyExpertForm('agent') })`。保存、失败保留表单、catalog refresh 和成功 toast 仍由既有 `ExpertEditor`/页面逻辑负责。

### 5. 可见字符串走现有 i18n 生成链路

新增“高级模式”和相关 aria/tooltip 文案应进入 source catalog，并补齐英文 override/catalog；不在组件内引入独立翻译机制。按钮和菜单只使用 ACECode 既有颜色 token，不写硬编码色值。

## Risks / Trade-offs

- [显式创建入口会覆盖同工作区未发送的 home draft] → 沿用现有 standalone expert opening-prompt 的显式导航优先级，只影响当前工作区；测试确保其它工作区 draft 不变且载荷只消费一次。
- [`expert-manager` 被用户禁用或资源缺失时 token 无法按 Skill 展开] → 不静默改写全局 Skill 策略；保留可见、未发送的 `/expert-manager `，并以现有 Seed/命令发现测试保证默认安装可用。
- [分段按钮出现误触或焦点丢失] → 主/箭头使用独立 button，补充点击边界、Escape、外部点击和焦点恢复的架构/交互测试。
- [菜单被 Desktop 原生网页表面遮挡] → 复用项目已有 overlay 标记和高层级 token 化 popup 样式。
- [一次性载荷在重渲染时再次覆盖输入] → 不改 `ChatView` 当前 consume 时序，并对“应用后消费、后续键入不被覆盖”建立回归断言。

## Migration Plan

该变更无数据迁移和后端部署顺序要求。发布时 Web bundle 与 i18n catalog 一起更新；回滚只需恢复原单按钮 handler，现有 `ExpertEditor`、home draft 和 session 数据均不受影响。

## Open Questions

无。用户已明确选择“预置 Skill、等待手动发送”，并明确高级模式继续使用原编辑器。
