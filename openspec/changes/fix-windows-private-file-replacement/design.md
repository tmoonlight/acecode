<!-- refactor-layout-map sha256:e2eb7cc27deba8a1e0bfb8fa3e6771094a4ccb875d33cab20578289cd5198965 -->
源码路径迁移请按 `scripts/refactor/src_layout_map.tsv` 换算；本设计中的历史路径保留。

## Context

现有文件 ACL 只有 GENERIC_READ 和 GENERIC_WRITE。Windows 重命名需要文件自身 DELETE 或父目录 FILE_DELETE_CHILD。发布验证使用仅有修改权限的目录，稳定复现写入返回 false；调试器确认异常来自 MCP 快照保存。

## Goals / Non-Goals

目标是保证私有文件在普通可修改目录下完成首次写入及后续原子替换，并维持仅当前用户访问。此次不改变并发串行化、临时文件命名及 POSIX 权限约定。

## Decisions

在当前用户的单一允许 ACE 中增加 DELETE。文件仍使用受保护 DACL，不继承其他主体的权限。测试给专属临时父目录设置排除 FILE_DELETE_CHILD 的权限，验证真实文件系统替换与文件 DACL。

## Risks / Trade-offs

当前用户获得删除自己的配置文件的权限，这是现有原子替换约定所需的最小补充。测试只修改自己的临时目录权限，并在析构时恢复后清理。

## Validation

先验证新回归在旧实现失败，再运行修复后的定向及完整原生测试、真实 MCP 守护进程检查和 Desktop/CLI 构建。
