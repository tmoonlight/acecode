# ACECode 专家头像规范

## 基本要求

- 主头像和状态头像均可选；没有真实图片时省略对应字段。
- 推荐 PNG，正方形，512×512 或 1024×1024。
- 推荐控制在 500 KiB 内，避免专家列表加载过慢。
- 文件放在专家包的 `avatars/` 下。
- `expert.json.avatar` 使用包内相对路径。
- 状态头像使用 `expert.json.stateAvatars`，只允许 `working`、
  `needs_attention`、`idle` 三个键。
- 支持 PNG、JPEG、GIF、WebP、BMP 和 ICO，单文件上限为 8 MiB。

## 状态语义与回退

- `working`：专家正在执行、编程或处理任务。
- `needs_attention`：专家向用户提问或等待确认。
- `idle`：专家没有操作或正在等待下一项任务。
- 任一状态没有专用图片时自动使用主 `avatar`；主头像也没有时保持无头像。
- GIF 原始字节必须完整保留，不进行缩放、转码、抽帧或静态化。

示例：

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

## 生成策略

先读取 Agent 指令，从中提取：

- 角色身份
- 专业能力的视觉符号
- 工作风格和气质
- 合适的背景色

再使用当前可用的图像生成工具。不要为不同专家复用完全相同的通用人物 Prompt。

示例：

```text
Professional editorial illustration avatar of a meticulous research analyst,
calm and focused expression, holding annotated source notes,
subtle evidence graph and document markers in the background,
bust portrait, facing forward, clean deep-teal background,
soft light, professional, natural, no text, square composition.
```

## Team 头像

Team 包只需要一张团队头像：

```text
avatars/team.png
```

从 Team 的 `displayDescription`、主理人定位和成员职业提取协作场景。不要复制任一
成员个人头像充当团队头像。

## 校验

生成后：

1. 确认文件真实存在且能打开。
2. 目视检查没有乱码、文字残片或明显畸变。
3. 必要时缩放和压缩。
4. 最后才把相对路径写入 `expert.json.avatar` 或 `expert.json.stateAvatars`。

如果图像工具不可用，省略头像并告诉用户可以以后补充；不要留下失效路径。
初始化专家骨架时同样不得创建状态头像占位文件或预写状态路径。
