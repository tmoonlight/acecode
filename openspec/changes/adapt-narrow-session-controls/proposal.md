## Why

桌面窗口变窄或右侧文件面板占宽时，目录标签逐字换行，待办进度和变更摘要溢出并遮挡操作。输入框工具栏收起标签后仍可能比聊天栏宽。

## What Changes

- 标题栏的文字保持单行，优先为操作和工作目录分配空间。
- 输入框按自身可用空间收起次要信息，在最小按钮仍放不下时允许分行操作。
- 底部摘要随聊天栏宽度减少统计细节，保留查看和关闭入口。聊天正文继续自然换行。

## Impact

- Affected specs: narrow-session-controls。此变更取代 prioritize-workspace-and-image-previews 中允许极长目录名换行的约定。
- Affected code: Web 会话标题、输入框工具栏、待办与变更摘要。
