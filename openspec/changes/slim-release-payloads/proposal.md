## Why

v1.0.3 Windows 包解压后 42.75 MiB，其中前端 HTML 原文 12.26 MiB、内置专家 PNG 头像约 3.68 MiB；macOS 更新 ZIP 为兼容独立 CLI 重复携带完整 daemon 和资源，增加约 18.3 MiB 下载量。用户要求压缩前端、改用像素办公室头像并关联专家子 agent、去除 macOS 更新包重复载荷。

## What Changes

- 构建时确定性压缩可获益的内嵌前端资源，保留原始内容兼容路径和开发目录服务。
- 将办公室现有人物模板和稳定外观算法提取为共享资源；内置专家使用同一像素头像身份，对应专家团子 agent 沿结构化专家绑定取得相同外观。
- 移除已替换的内置专家大 PNG，升级种子清单，并保留用户自定义头像。
- macOS 更新 ZIP 只携带一份完整应用和资源，以小型、签名且随应用公证的 CLI 转发入口维持旧版独立 CLI 的平铺更新兼容性。
- 增加体积、资源完整性、头像身份关联和旧版更新路径回归检查。

## Capabilities

### New Capabilities

- `compressed-web-assets`: 内嵌前端压缩存储、HTTP 编码协商和无损兼容。
- `expert-office-avatars`: 专家和其子 agent 使用与像素办公室一致的稳定头像。

### Modified Capabilities

- `self-upgrade`: macOS 更新包去重，同时保留桌面与独立 CLI 升级入口和签名验证。

## Impact

涉及 CMake 资源生成器、静态资源接口与 HTTP 路由、前端专家和子 agent 展示、办公室内嵌页面、专家种子资源和元数据投影、macOS CLI 转发入口及打包验证。沿用现有 zlib 依赖；生成器使用 Python 标准库。保留当前分支、工作区其他改动，不发布或改变既有标签。
