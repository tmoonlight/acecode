---
name: understand-chat
description: 用项目知识图谱回答架构、调用路径、模块职责和依赖问题，并回到源码核对。
---

# /understand-chat

如果只读会话无法启动本地查询脚本，可用 ACECode 的文件读取工具按需查看图谱和源码。小图可直接读取，较大的图先按符号搜索。

PACKAGE 是本 SKILL.md 所在目录向上两层的绝对路径。通过技能调用中的路径或技能列表定位它。以下命令在目标项目运行，路径均作为单独参数传入。只有 python3 的系统使用 python3。

ARG 是用户的问题。运行 `python "PACKAGE/scripts/ua.py" query "项目路径" "关键词"`（概况查询省略关键词），先不带关键词查看项目概况，再按问题里的文件名、符号或相关模块分别查询。沿返回的 source、target 追踪相邻节点，回到 filePath 和 lineRange 核对关键关系。概念性问题用实际模块或符号重试。无图谱时引导运行 /understand。检查 git status 与图谱提交时间，在有相关新改动时说明证据对应的版本。用用户的语言直接回答，附源码位置。
