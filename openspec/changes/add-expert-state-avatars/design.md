## Context

专家包当前用 `expert.json.avatar` 保存一条包内相对路径。`ExpertRegistry` 加载后把它解析为受包根约束的文件路径，Web DTO 只公开 `/api/experts/<id>/avatar` URL；该端点有 8 MiB 上限，允许 PNG、JPEG、GIF、WebP、BMP、ICO，并直接返回文件字节。GIF 因此已经可以作为主头像原样传输，但状态头像没有 schema、模型、URL 或编辑语义。

`ExpertDraft` 和 Web `ExpertEditor` 当前不建模主头像，原子更新通过复制现有专家包来保留未编辑的头像和其它资源。`expert-manager` Seed Skill 则直接创建/修改专家目录，并拥有 `expert.json`、头像规范和 Python 校验脚本。第一阶段需要贯通这些数据边界，同时明确不推断运行时状态、不做自动切换。

## Goals / Non-Goals

**Goals:**

- 为 Agent 和 Team 的专家 manifest 建立三个固定、可选的状态头像引用：`working`、`needs_attention`、`idle`。
- 在 Registry、Draft、HTTP DTO、头像读取、Web 管理编辑和 `expert-manager` 中使用一致语义，并保留未建模资源与未来扩展。
- 对缺少状态头像的请求统一回退主头像；主头像也没有时保持现有无头像结果。
- 对状态头像执行包内路径、存在性、普通文件、大小和 MIME/扩展名检查，并保证 GIF 字节不被转码或抽帧。
- 使旧专家包无需迁移，更新其它字段时也不会丢失已有状态头像或图片文件。
- 在项目待办中记录后续“根据子代理运行/提问/空闲状态选择有效头像”的未完成工作。

**Non-Goals:**

- 不在本阶段监听 AgentLoop、subagent、AskUserQuestion、权限确认或 idle 事件，也不自动改变任何已渲染头像。
- 不定义动画播放节奏、状态优先级、状态机或跨会话聚合规则。
- 不在 Web Editor 内上传、生成、缩放或转码图片；编辑器只配置专家包中已经存在的相对路径，图片资产由 `expert-manager`、导入流程或人工包管理放入包内。
- 不改变现有 `avatar` 字段、主头像 URL、主头像可用类型或旧包发现优先级。

## Decisions

### 1. Manifest 使用 `stateAvatars` 对象，API 使用 `state_avatars`

manifest 新增可选对象：

```json
{
  "avatar": "avatars/default.png",
  "stateAvatars": {
    "working": "avatars/working.gif",
    "needs_attention": "avatars/needs-attention.png",
    "idle": "avatars/idle.png"
  }
}
```

三个已知键的值必须是非空、包内相对路径。对象或任一键均可省略；省略键表示该状态使用主头像。HTTP/前端请求和响应使用项目现有 snake_case DTO 约定 `state_avatars`。不新增三个顶层字段，因为对象能保持 schema 边界并为未来状态扩展保留命名空间。

现有 manifest 中 `stateAvatars` 的未知键在更新时保留但不进入当前运行时 DTO；来自当前 API payload 的未知键应被拒绝，以发现拼写错误。这样兼顾前向兼容与当前写入的严格性。

### 2. Draft 使用 presence 语义，更新只管理三个已知键

`ExpertDefinition` 持有经过校验的包内引用及解析后的文件路径；`ExpertDraft` 持有可选的三个相对引用，并记录 payload 是否包含 `state_avatars`/`stateAvatars`。

- 创建或更新 payload 未包含该对象：不管理状态头像；更新时完整保留现有对象和文件。
- payload 包含该对象：对象对三个已知键是权威值；非空键设置/替换引用，省略的已知键被清除。
- 空对象：清除三个已知引用，但复制到 staging 的图片文件仍保留，避免“解除引用”被误当成删除资产。
- materialize 时合并已知键并保留对象内未知扩展键以及 manifest 其它未知字段。

使用 presence bit 而不是默认空 map，可避免现有 Web 编辑器或旧客户端在更新其它字段时意外清空新数据。

### 3. 配置路径在保存/发现时严格校验，读取竞态才回退

每个已配置路径必须：

- 是相对路径，规范化后仍位于专家包根内；
- 指向真实普通文件，不能是目录、逃逸路径或符号链接到包外；
- 使用 PNG、JPEG、GIF、WebP、BMP、ICO 之一，并符合现有 8 MiB 头像上限。

允许类型应抽成专家头像共享 helper，由 Registry、HTTP serving 和 `expert-manager` 校验保持同一列表，避免状态端点复制一套漂移的白名单。出于安全与一致性，manifest 明确引用不存在或不支持的文件属于无效配置并产生诊断/保存错误；“缺失回退”指未配置状态键。若文件在成功发现后到 HTTP 读取之间被外部删除或暂时不可读，端点应尝试主头像作为竞态回退，而不是返回不受控文件或崩溃。

### 4. 保留主头像端点，并用受限 state 查询解析有效头像

现有 `GET /api/experts/<id>/avatar?workspace=...` 继续返回主头像。新增可选 `state=working|needs_attention|idle`：

- 状态已配置且文件可读时返回该状态图片；
- 状态未配置时返回主头像；
- 已验证状态图片在读取时瞬时不可用时尝试主头像；
- 状态和主头像都不可用时返回现有 404；
- 未知 state 值返回 400，不静默映射。

响应继续直接流式/读取原文件字节并设置准确 `Content-Type`、`nosniff` 和私有缓存头。不得通过 canvas、图片库或静态缩略图处理 GIF，因此动画数据完整保留。

普通专家 DTO 保留 `avatar_url` 并新增 `state_avatar_urls`，为三个固定状态提供“有效头像”URL（只有主/状态头像至少一个可用时生成）。受管理专家的详情 DTO 另提供安全的 `state_avatars` 相对引用供编辑；不得返回解析后的主机绝对路径。

### 5. Web 编辑器只管理包内引用，不承担图片上传

Web 数据归一化、form 和 payload 增加三个可选值。受管理专家编辑界面展示一个“状态头像（可选）”分组，明确提示路径必须先存在于专家包 `avatars/` 等目录中，并为每个状态提供相对路径输入、当前有效预览与清除操作。Agent 和 Team 共用该分组。

编辑器提交完整的已知状态对象，从而能清除单个引用；服务端仍是路径安全和文件类型的权威校验者。服务端失败必须保持表单值和错误信息。第一阶段不增加 file input 或二进制上传 API，以免把原子专家 JSON 更新扩展为尚未设计的临时上传事务。

### 6. `expert-manager` 是创建图片资产的规范入口

同步 `expert-json-spec.md`、`avatar-spec.md`、Skill 主流程和 Python validator：说明三种状态、命名、主头像回退、允许类型与 GIF 原样保留。初始化脚本不创建占位图片或失效路径；用户提供/生成真实资产后才写字段。批量/校验路径接受新对象，拒绝未知当前输入键、逃逸、缺失文件和不支持类型。

修改 Seed Skill 后必须更新 `assets/seed/MANIFEST.json` 的哈希/来源版本及 `seed.version`，并保留默认 Skill seeder 的 ownership-safe 升级语义，不能覆盖用户修改过的 Skill。

### 7. 动态状态选择明确留给后续 change

本 change 完成且验证后，只在项目唯一待办入口 `docs/ACECode 待办.md` 清单末尾追加一个未完成编号，描述未来把 subagent/Agent 运行、等待用户和空闲状态映射到三种头像。该待办不得标记完成，也不得在本 change 中加入临时轮询或硬编码状态切换。

## Risks / Trade-offs

- [Web 编辑器不能直接上传图片] → 第一阶段定位为数据/持久化能力，提供清晰的包内路径和预览；由 `expert-manager` 或导入流程安全放置原文件，二进制上传另行设计。
- [旧客户端更新时清空状态头像] → 使用 `state_avatars_present` 区分“未发送”与“明确空对象”，并建立 lossless update 测试。
- [manifest 路径与 API URL 泄露主机信息] → DTO 只返回包内相对引用和认证后的 HTTP URL，绝不序列化解析后的绝对路径。
- [GIF 被前端或后端静态化] → 不引入解码/转码依赖，HTTP 测试比较原始字节并验证 `image/gif`。
- [新白名单与主头像行为漂移] → 状态头像复用现有可服务 MIME 集合和大小上限；不趁机扩大或收紧主头像契约。
- [Team 的动态状态语义尚未定义] → 数据字段先对 Agent/Team 对称持久化，状态优先级和团队聚合留到后续 spec。
- [外部删除已配置资产导致发现或读取失败] → 保存/扫描时严格诊断；成功发现后的读取竞态只回退经过同样安全校验的主头像。

## Migration Plan

无需批量改写旧包。缺少 `stateAvatars` 的包按三种状态全部回退现有 `avatar`。新版本先部署兼容读取和 DTO，再由 Editor/`expert-manager` 写入字段；旧版本会把 `stateAvatars` 当未知 manifest 数据保留。回滚时新字段和图片仍留在包内，旧版本继续使用主头像。

## Open Questions

无。运行时状态来源、优先级、动画展示和 Team 聚合规则明确不属于本阶段，将由待办中的后续 OpenSpec change 决定。
