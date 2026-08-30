---
name: vision-image-reader
description: ONLY for models that cannot see images. If "# Environment" says "Active model can read images directly: Yes", read the image yourself and do not use this skill. Otherwise, delegate image reading to a vision-capable saved model.
---

# Vision Image Reader

把图片理解委托给另一个带 `vision` 能力标签的保存模型。**这个 skill 只服务于看不见图片的模型。**

## 先判断你是否需要它

system prompt 的 `# Environment` 段有一行:

```
- Active model can read images directly: Yes | No
```

- **Yes** → 你自己就能看图。**不要用这个 skill,也不要调 `vision_analyze`。** 会话里已有的图片附件你直接就能看见;磁盘上的图片文件用 `show_image` 工具拉进上下文,下一轮你就能看见它。此时调用 `vision_analyze` 会被直接拒绝(`ACTIVE_MODEL_HAS_VISION`)。
- **No** → 图片附件到你这里会变成一段文字句柄而不是图片本身。继续往下走。

不要靠"我觉得我看得够不够可靠"来判断 —— 以上面那一行为准。

## Workflow

1. 判断是否真的需要图片内容。只在需要读取截图、照片、界面图、错误弹窗、图表、扫描件等视觉信息时使用。
2. 调用 `vision_analyze` 工具。
3. `prompt` 写清楚要视觉模型回答什么。保持通用,不要把业务流程写死在 skill 里。
4. 选择图片来源(按优先级):
   - 工具(浏览器截图、shell 等)已经把图片写到了本地路径 → 传 `image_path`。相对路径相对当前工作目录解析,绝对路径可以在 workspace 之外。工具会读取并校验它是图片,再保存为会话附件后发送。
   - 用户本轮或最近一轮只上传了一张图 → 省略其它参数,工具会用当前会话里最新的图片附件。
   - 要指定某张已上传的图 → 传 `attachment_id`。
5. 如果要指定某个视觉模型,传 `model_name`;该保存模型必须带 `vision` 能力标签,且不能是当前模型自己。未指定时工具会从带 `vision` 标签的其它保存模型里选择一个。
6. 使用工具返回的图片理解结果继续主任务。不要把 `vision_analyze` 的内部调用当作可 resume 或可在界面里切换的会话。

## Tool Shape

```json
{
  "prompt": "Describe the screenshot and identify the visible error.",
  "image_path": "optional/path/to/screenshot.png",
  "attachment_id": "optional-image-attachment-id",
  "model_name": "optional-saved-model-name"
}
```

## Guidance

- 当前模型不能看图时,如果消息里带了图片附件,主模型会收到一段说明而不是图片本身,提示用 `vision_analyze` 分析。按这个提示走即可。
- 如果返回 `ACTIVE_MODEL_HAS_VISION`,说明你自己就能看图。不要重试、不要换模型名再试 —— 直接读会话里的图片,或用 `show_image` 把磁盘上的图片拉进上下文。
- 如果 `vision_analyze` 返回 `NO_VISION_MODEL`(没有可用视觉模型),提示用户到模型设置里给一个保存模型勾选 `视觉` 能力,不要反复重试。
- 如果返回 `NO_IMAGE_ATTACHMENT`(没有图片附件),请用户上传图片,或提供明确的 `attachment_id` / `image_path`。
- 只有图片文件才能传给 `vision_analyze`:`image_path` 指向不存在的路径、目录、超大文件或非图片文件(包括 PDF、SVG 等非栅格图)会被清晰拒绝。不要把普通文档 / 二进制文件当成视觉输入。
- 对多图任务,分别调用工具读取每张图,再在主会话中综合结果。
