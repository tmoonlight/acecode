---
name: understand-onboard
description: 根据图谱、架构层和导览，为新加入项目的开发者生成上手路线。
---

# /understand-onboard

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

先运行不带搜索词的 `python "PACKAGE/scripts/ua.py" query "项目路径" "关键词"`（概况查询省略关键词），读取 project、layers、tour。按导览引用的节点 ID 查询并打开对应源码，优先选启动入口和一条端到端功能路径。结合当前 README 提供可执行的启动或测试命令。按读者背景给出一条简短阅读顺序，附路径。无图谱时引导运行 /understand。
