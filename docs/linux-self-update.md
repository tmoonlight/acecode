# Linux 自升级发布与旧版引导

ACECode 的 Linux 下载包同时保留 `.tar.gz` 和自升级专用 ZIP：

- `acecode-linux-x64.tar.gz` / `acecode-linux-arm64.tar.gz` 用于手动下载、兼容安装和 npm 组包。
- `acecode-<version>-linux-x64-update.zip` / `acecode-<version>-linux-arm64-update.zip` 仅用于 `acecode update`。

Linux updater ZIP 必须由 Linux runner 生成。ZIP 内保留唯一的 `acecode-linux-<arch>` 顶层目录以及 Unix external attributes；最终产物必须重新解包并验证 `acecode`、`acecode-desktop` 执行位、精确版本、models.dev 三个资源文件和不存在 `ace-browser-*` 退役产物。

## Manifest 目标与兼容边界

Linux 自升级 ZIP 只允许写入以下能力版本化目标：

```text
linux-x64-updater-v1
linux-arm64-updater-v1
```

不得把 ZIP 登记到旧的 `linux-x64` 或 `linux-arm64` 目标。`v0.8.8` 及更早版本的解压器不会从 ZIP 恢复 Unix 执行位；旧客户端若收到这种包，可能把安装目录中的 `acecode` 替换成不可执行文件。`v0.8.9` 加入权限恢复，但在能力目标推出前发布的 Linux 客户端仍需按下节完成一次手动引导。

manifest 继续使用 schema 1。新稳定版条目形状如下：

```json
{
  "target": "linux-arm64-updater-v1",
  "file": "acecode-<version>-linux-arm64-update.zip",
  "sha256": "<lowercase-64-hex>",
  "size": 12345678
}
```

## 从旧 Linux 版本做一次性引导

旧版显示“已经是最新”时，先不要向旧 manifest 目标临时添加 ZIP。应从 GitHub HTTPS Release 下载最新稳定 tarball 和同一 Release 的 `SHA256SUMS.txt`，校验后用原安装方式替换程序。下面命令只负责下载、校验和解包，不会自动覆盖现有安装：

```bash
set -eu

case "$(uname -m)" in
  x86_64|amd64) asset="acecode-linux-x64.tar.gz" ;;
  aarch64|arm64) asset="acecode-linux-arm64.tar.gz" ;;
  *) echo "Unsupported Linux architecture: $(uname -m)" >&2; exit 1 ;;
esac

release_url="$(curl -fsSL -o /dev/null -w '%{url_effective}' \
  https://github.com/tmoonlight/acecode/releases/latest)"
version="${release_url##*/v}"
work_dir="$(mktemp -d)"
echo "Download ACECode v${version} into ${work_dir}"

curl -fL "${release_url}/download/${asset}" -o "${work_dir}/${asset}"
curl -fL "${release_url}/download/SHA256SUMS.txt" \
  -o "${work_dir}/SHA256SUMS.txt"
(
  cd "${work_dir}"
  grep -F "  ${asset}" SHA256SUMS.txt | sha256sum -c -
)
tar -xzf "${work_dir}/${asset}" -C "${work_dir}"
echo "Verified package: ${work_dir}/${asset%.tar.gz}"
```

停止正在运行的 ACECode 进程后，先备份原安装目录，再把已验证目录中的内容按原安装权限复制回去；`acecode` 和 `acecode-desktop` 必须保持 `0755`。如果原目录需要管理员权限，只对明确的安装目标使用 `sudo`，不要对 `~/.acecode` 用户数据目录做覆盖。完成后验证：

```bash
acecode --version
acecode update
```

只有安装了包含 `linux-*-updater-v1` 支持的稳定版后，后续 `acecode update` 才能沿新目标自动升级。

## 稳定版发布门禁

Tag 工作流完成后，对两个 Linux updater ZIP 逐一执行：

1. 从 GitHub Release 下载最终 ZIP，并与 `SHA256SUMS.txt` 核对。
2. 解包验证唯一顶层目录、`test -x acecode`、`test -x acecode-desktop`、`acecode --version` 与 Tag 一致。
3. 复制到 `J:\jenkins_green\aupdate` 时先使用临时文件名，完成大小和 SHA-256 核对后再原子改名。
4. 把准确的文件名、SHA-256 和大小写入同一稳定版的 `aceupdate.json`，目标只能是 `linux-*-updater-v1`。
5. 通过公网 URL 重新下载 ZIP 和 manifest，核对 HTTP 200、`application/zip`、Content-Length、SHA-256 以及 manifest 字段。
6. 在已安装支持版的真实 Linux x64 与 ARM64 环境分别运行 `acecode update`，确认新版本可执行、用户配置未被覆盖且备份目录存在。

任何一步失败时，从 manifest 移除对应 `updater-v1` 条目即可停止分发；保留 tarball 供人工恢复，不要改回不安全的旧目标。
