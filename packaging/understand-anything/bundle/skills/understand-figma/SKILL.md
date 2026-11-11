---
name: understand-figma
description: 通过 Figma REST API 分析设计文件、页面、组件、实例和设计令牌，生成设计图谱。
---

# /understand-figma

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

将 FIGMA_TOKEN 作为环境变量提供给 ACECode。不要把令牌写入聊天、命令参数或图谱。确认用户指定的 Figma URL 或 file key 和用于保存设计图的项目目录；已有另一类型图谱时选择专用目录。执行 `python "PACKAGE/scripts/special.py" start figma "项目" "Figma URL 或 key"`。up-to-date 表示图谱已可使用。否则读取返回的 context，按页面分组，依据 PACKAGE/agents/design-analyzer.md 和请求语言分析组件用途、命名与关系，写入 work/analysis-batch-N.json。可使用当前可用的子代理或依次分析。执行 `python "PACKAGE/scripts/special.py" finish "run路径"` 发布图谱。真实抓取需联网，读取环境变量中的 token。
