# 字符串工具统一重构方案

> **状态:未启动。** 本文只记录方案与现状盘点,不代表已实施。
> 触发案例见 commit `a863aad`(非 ASCII 路径下自升级解压失败)。

## 为什么做

2026-08-30 线上问题:用户目录含中文时,自升级在解压阶段必定失败,界面报
`invalid package: failed to open zip package`,重试无效,只能手动覆盖绿色包。

根因是 [src/upgrade/package.cpp](../src/upgrade/package.cpp) 把 `zip_path.string()`
交给了 `zip_open`。MSVC 的 `path::string()` 返回当前本地代码页(中文系统为 GBK)
的字节,而 libzip 在 Windows 上把该参数当 UTF-8 严格解析,转换失败即返回 NULL。

值得注意的是**同一仓库的 feedback 上传模块早就写对了**
(`zip_open(path_to_utf8(...))`),`path_to_utf8` / `path_from_utf8` 也已经在 759 处
被正确使用。问题从来不是"没有工具",而是**工具存在却可以被绕过,且绕过时不报错**:
`path.string()` 编译得过、跑得通、在纯 ASCII 路径下测试全绿,只在中文用户的机器上炸。

这与 [postmortem-file-link-preview.md](postmortem-file-link-preview.md) 得出的教训
是同一条:**不能依赖"每个调用点都记得做对"**,判据要收进函数内部,并用护栏兜住。
那次是路径的百分号编码,这次是路径的字节编码 —— 同类问题已经重复发生两次。

因此本方案的重心是**护栏**,而不是"少写几个 trim"。

## 现状盘点

盘点时间 2026-08-30,数据来自 `src/` 全量扫描。

### 第一层:编码与路径转换

| 现象 | 数量 |
|---|---|
| 正确入口 `path_to_utf8` / `path_from_utf8` 的使用 | 759 处 |
| 各自直接调 Win32 转换 API(`MultiByteToWideChar` / `WideCharToMultiByte`)的文件 | 9 个 |
| 绕过入口、直接把 `.string()` 交给 C 接口的残留 | 本次修掉 2 处,apply/main 仍有少量 |

9 个直接调 Win32 转换的文件:`daemon/service_win.cpp`、`desktop/context_picker.cpp`、
`desktop/folder_picker_win.cpp`、`desktop/splash_screen.cpp`、
`network/proxy_resolver_win.cpp`、`utils/cwd_hash.cpp`、`utils/encoding.cpp`、
`utils/terminal_title.cpp`、`utils/text_file_buffer.cpp`。

### 第二层:基础字符串处理

| 函数 | 定义处 | 备注 |
|---|---|---|
| `trim` / `trim_copy` / `ltrim` | **60 处**,散在 22+ 个文件 | 三种不一致语义 |
| `split` / `split_lines` | 4 个文件各写一份 | — |
| `to_lower` / `to_upper` | 2 个文件 | — |

三种 trim 语义:

```cpp
// A. std::isspace —— 受 locale 影响              (commands/model_command.cpp)
// B. " \t\r\n" —— 不含 \v \f                     (network/proxy_resolver.cpp
//                                                 gitinfo/git_context_collector.cpp
//                                                 experts/expert_registry.cpp)
// C. " \t\r\n\v\f" —— 六字符全集                 (session/todo_state.cpp)
```

### 第三层:已有资产

`encoding.hpp`(145 行)、`utf8_path.hpp`(42)、`base64.hpp`(140)、`url_encoding.hpp`(31)、
`stream_processing.hpp`(120)、`text_input_ops.hpp`(99)。

其中 UTF-8 边界处理**已经散在三个头里**且功能重叠:`encoding.hpp` 的
`truncate_utf8_prefix` / `truncate_utf8_suffix` / `trim_trailing_partial_utf8`、
`stream_processing.hpp` 的 `utf8_safe_boundary`、`text_input_ops.hpp` 的
`clamp_utf8_boundary`。这部分是真正值得合并的。

## 目录设计

```
src/utils/str/
├── str_basic.hpp           纯 header,零依赖  trim/split/join/case/starts_with/replace_all
├── str_utf8.hpp            纯 header        码点计数、安全截断、边界、校验
├── str_encoding.hpp/.cpp   平台相关         utf8↔wide↔codepage、getenv_utf8、ensure_utf8
├── str_path.hpp            纯 header        path ↔ utf8(现 utf8_path.hpp 原样搬)
└── strings.hpp             门面,聚合 include
```

**为什么分四个文件而不是合成一个 StringUtil**:`str_encoding` 必须带 `.cpp`(平台代码),
若与 `trim` 同处一个头,任何只想 trim 一下的模块都会被迫链接平台编码代码。这不是假想
——本次修复给 `sha256.hpp` 引入 `path_to_utf8` 时就撞上了这个权衡:它原本是零链接依赖
的纯头文件。分层能把这种传染挡住。

调用方日常只 `#include "utils/str/strings.hpp"`,不需要记住哪个能力在哪个文件里。

## 最大的风险:语义统一

60 处 trim 语义不一致,**盲目全局替换会引入回归**。处理办法:

1. 标准定为 `" \t\r\n\v\f"` 六字符集,**不用 `std::isspace`** —— 它受 locale 影响,
   对 >127 的字节行为依赖运行环境,而我们处理的是 UTF-8 字节流。
2. 提供两个形态:`trim(s)` 用标准集,`trim(s, chars)` 供有特殊需求的调用点显式传字符集。
3. 替换时逐个确认差异:
   - A 类(`isspace`):项目未调用 `setlocale`,运行在 C locale 下,与六字符集**等价**,安全。
   - B 类(`" \t\r\n"`):替换后会**多** trim 掉 `\v` `\f`。这两个字符在配置值、git 输出、
     命令参数里几乎不可能出现,但属于行为变化,替换时要过一眼。

这条决定了迁移必须**按模块小批推进**,不能一次全局替换。

## 迁移批次

| 批次 | 内容 | 风险 | 估时 |
|---|---|---|---|
| 1 | 建目录,把现有四个头搬进去;旧路径保留转发头 | 零 —— 纯搬家,调用点一行不改 | 半天 |
| 2 | **加护栏测试**(见下节) | 零 —— 只加测试 | 1–2 小时 |
| 3 | 合并三处重叠的 UTF-8 边界函数到 `str_utf8.hpp` | 低 —— 有现成单测 | 2 小时 |
| 4 | 按模块替换 trim/split,每模块跑一遍自身测试 | 中 —— 见语义风险一节 | 每模块 15–30 分钟 × 22 |
| 5 | 收敛散落的 Win32 转换(9 个文件中的 6 个) | 低 | 半天 |

**批次 2 要提到批次 4 之前做** —— 护栏先立起来,后面替换才有兜底。

## 护栏(方案里最值钱的部分)

仓库已有 60+ 个 `web/src/lib/*Architecture.test.js`,用 Node 读 C++ 源码做正则断言
(`agentBrowserArchitecture.test.js` 就是扫 `src/` 确保宽松 TLS 只出现在两个文件里)。
直接复用这个模式,新增 `web/src/lib/stringEncodingArchitecture.test.js`:

```js
run('libzip 调用点不得使用本地代码页路径', () => {
  const pkg = source('src/upgrade/package.cpp');
  assert.match(pkg, /zip_open\(path_to_utf8\(/);
  assert.doesNotMatch(pkg, /zip_open\([^)]*\.string\(\)/);
});

run('平台编码转换只允许出现在指定文件', () => {
  const allowed = ['src/utils/str/str_encoding.cpp',
                   'src/utils/text_file_buffer.cpp'];  // 文件编码探测,合理特例
  for (const f of cppSources()) {
    if (allowed.includes(f)) continue;
    assert.doesNotMatch(source(f), /MultiByteToWideChar|WideCharToMultiByte/,
                        `${f} 应改用 str_encoding 的统一入口`);
  }
});

run('trim 只允许有一份定义', () => { /* 扫 src/ 下的 std::string trim( 定义 */ });
```

**能力边界要诚实**:前两条能可靠检查,模式明确。`fs::path(std::string)` 这类构造想靠
正则全面拦截会误报 —— 变量名千变万化。所以第三层防线是 `str_path.hpp` 的注释约定加
code review 规则,而不是假装正则能全覆盖。

## 明确不做的

- **不碰 `base64.hpp` / `url_encoding.hpp`** —— 它们是编解码算法,不是字符串处理,
  保持独立更清晰。
- **不碰 `text_file_buffer.cpp` 的 13 处转换** —— 那是读取 GBK/UTF-16 文件时的编码探测,
  业务特例,强行收敛只会让它更难读。
- **不做 `string_view` 全面改造** —— 改动面大、收益小,不在本次范围内。
- **不做文案集中 / i18n** —— 那是另一条线:C++ 侧有 750 处硬编码中文,而
  `src/desktop/strings.cpp` 已有成熟机制(enum ID + zh-CN/en-US 双目录 +
  `static_assert` 等长 + locale 解析与运行时切换),覆盖 52 条。若要做,方向是把
  desktop 那套提升为全局 `acecode::strings`,而不是新造。参见
  [localization.md](localization.md)。

## 验收标准

- 批次 1–3 完成后:`acecode_unit_tests` 全绿,`pnpm test` 全绿,无调用点改动。
- 批次 4 每完成一个模块:该模块自身测试全绿。
- 批次 5 完成后:护栏测试中的 `allowed` 白名单不超过 2 个文件。
