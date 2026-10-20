## Why

ACECode 安装包发布后，公开下载站目前需要人工运行 Pages 工作流。
将刷新接入发布成功后的流程，使下载目录在本机离线时也能自动更新。

## What Changes

- `package.yml` 的 Release 发布成功后调用独立的网站同步工作流。
- 同步工作流通过仅限 `acecode-site` 的部署密钥更新公开清单并触发 Pages。
- 校验目标版本已发布且安装资产具有校验值；重复执行不创建空提交。
- 同步后等待 CDN 公开清单与提交内容一致，失败在发布流水线中明确显示。

## Capabilities

### New Capabilities

- `public-release-catalog`: 公开下载清单随 GitHub Release 发布自动同步、部署和验证。

### Modified Capabilities

无。

## Impact

涉及 ACECode 的 GitHub Actions 和发布文档，以及独立仓库
`tmoonlight/acecode-site` 的清单生成器、测试与部署文档。
本机 aupdate 镜像流程、用户中心和安装包构建行为保持原有约定。
