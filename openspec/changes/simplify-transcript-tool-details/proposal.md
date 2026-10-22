# Proposal

## Why

工具 transcript 混用英文动词和原始工具名，参数直接拼接且缺少含义，右侧统计挤占主行。用户确认统一为「动作：最小必要对象」，展开后补充信息，避免重复。

## What Changes

- 全部内置工具提供随界面语言切换的中文、英文名称，覆盖运行、完成、失败和历史记录。
- 工具行使用冒号，文件对象仅显示文件名；参数和统计在展开卡片中显示。
- 展开内容按工具选择必要字段，保留完整路径、原始结果与复制能力；命令不重复展示，不增加冗余 Shell 标题或参数卡。
- 保留现有展开箭头的尺寸、位置、旋转、hover / 键盘焦点显隐设计。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `web-chat-activity-collapse`: 增加单个工具行的本地化、精简对象和展开详情去重要求。

## Impact

Web/Desktop 共用的 ToolBlock、ActivityLine、ShellCommandCard、transcript 投影、翻译目录及测试。仅改变展示，不更改工具协议、执行逻辑、存储内容和 TUI。保留当前工作区的其它改动。
