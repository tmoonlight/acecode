## Context

`package.yml` 通过工作流令牌调用 GitHub Release 发布；该令牌产生的普通事件
不能用作另一个工作流的可靠触发器。下载站位于独立公开仓库
`tmoonlight/acecode-site`，已有 push 驱动的 Pages 工作流。

## Goals / Non-Goals

目标是将目录更新、Pages 部署与 CDN 验证串联为发布后续步骤。
不改变安装包构建矩阵、版本生成、用户中心或本机 aupdate 镜像。

## Decisions

1. 在 `package.yml` 添加 `needs: release` 的可复用工作流调用。
   不依赖由工作流令牌生成的 `release.published` 事件，也不使用定时轮询。
2. 新工作流支持 `workflow_call` 和维护用 `workflow_dispatch`，输入明确标签。
   它检查独立网站仓库 main，生成公开目录、校验、精确暂存并 push。
3. 网站仓库使用专用 SSH 写入部署密钥；私钥仅存 ACECode Actions Secret。
   其权限局限于网站仓库，ACECode 的工作流令牌仅保留读取权限。
4. 清单生成器提供只同步 Release 与必需标签校验参数，避免此步骤依赖价格服务。
   记录目标版本公开来源，首次接入可用现有版本验证整条链路。
5. 网站 push 自动部署 Pages。同步工作流比较 CDN `aceupdate.json` 的 SHA-256，
   最多等待 15 分钟，覆盖 Pages 构建时间与已有 5 分钟边缘缓存。
   相同内容不创建空提交，但仍验证线上目录。
6. 同步工作流串行执行，push 遇到外部并发更新重新拉取并生成，最多重试三次。
   仅刷新清单及来源记录，不推送价格数据或其他页面内容。

## Risks / Trade-offs

- Pages 或 GitHub 故障 → 同步步骤失败，保留已发布资产，支持重跑。
- 网站清单已更新但 Pages 上一次部署失败 → 幂等同步不制造空提交；修复网站工作流后重跑 Pages，再重跑验证。
- 两次发布快速连续完成 → 每次读取完整公开 Releases，避免覆盖最新版本。
- 密钥失效 → Actions 显示认证失败，替换网站部署密钥及对应 Secret。

## Migration Plan

先发布网站生成器与测试，再配置专用密钥，通过 PR 将 ACECode 工作流合入 master。
用当前已发布版本调用同一可复用工作流，验证网站 push 自动启动 Pages 及 CDN 内容。
回退时移除发布后的工作流调用并撤销该部署密钥；既有下载站继续工作。
