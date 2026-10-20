# 公开下载站自动更新

GitHub 标签发布成功后，`package.yml` 自动调用
`refresh-public-downloads.yml`，更新 `tmoonlight/acecode-site` 的下载清单。
网站仓库的 push 自动触发 Pages 部署，发布任务继续等待 CDN 清单内容一致。
正常发布无需在本机执行网站刷新命令。

同步只接受已公开且核心安装资产及 SHA-256 完整的版本。普通分支构建、
失败的 Release 发布不会触发网站更新。重试会读取全部公开 Releases，
保留最新版本；相同内容不产生空提交。

跨仓库写入使用 `PUBLIC_SITE_DEPLOY_KEY` Actions Secret，对应部署密钥
仅授权 `acecode-site` 仓库。不要在代码、日志或普通变量中放置私钥。

流水线的网站同步失败时，已发布的安装包不回滚。检查同步任务和网站仓库
`Deploy Pages` 的失败原因，修复后重跑失败任务。维护时也可在 ACECode
仓库 Actions 中选择 `refresh-public-downloads`，输入已有 `v<version>` 标签。
CDN 使用短缓存，工作流最多等待 15 分钟确认公开目录。

该步骤不替代原有的本机 aupdate 完整镜像与客户端升级验证。
