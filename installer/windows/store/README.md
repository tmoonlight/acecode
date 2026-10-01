# Microsoft Store MSIX

使用当前 Windows Release 构建生成原生 Win32 MSIX，包含 Desktop、CLI、Computer Use、WinPTY、模型目录、seed 和许可文件。需要 Python 3.11+、Pillow、Windows SDK MakeAppx。

本地验证（不需要账号标识，不可提交商店）：

```powershell
python installer/windows/store/build_msix.py --build-dir build --configuration Release --local-test
```

正式包使用 Partner Center 的 Product identity，另存 UTF-8 JSON：

```json
{
  "name": "填写 Package/Identity/Name",
  "publisher": "填写 Package/Identity/Publisher",
  "publisher_display_name": "填写 Package/Properties/PublisherDisplayName"
}
```

```powershell
python installer/windows/store/build_msix.py --build-dir build --configuration Release --identity-file C:/path/to/store-identity.json
python tests/scripts/build_msix_test.py
```

产物写入 build/store-msix 下独立目录，不覆盖旧包。每次输出 MSIX、MakeAppx 日志、逐文件哈希及 package-report.json。报告记录源码提交和未提交修改；正式交付应来自已核验的提交。

项目版本 a.b.c 映射为商店版本 (a+1).b.c.0，因此 0.9.30 对应 1.9.30.0；不要在后续版本改变此规则。商店安装保持应用内版本不变。

本地包使用 ACECode.LocalValidation 身份和 acecode-store-test.exe 别名，避免覆盖正式命令行入口。MSIX 未做本机可信签名；MakeAppx 校验成功不代表本机安装或商店认证通过。提交商店无需购买代码签名证书，商店认证后会签名。安装实测、产品标识、隐私 URL、审核凭据与提交状态在 docs/microsoft-store 中另行记录。
