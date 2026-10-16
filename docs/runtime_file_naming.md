# 运行数据命名

ACECode 自动生成的运行数据名称使用下划线 `_` 分隔。此规则包括会话 ID、日期日志、升级诊断、反馈包、导出建议名、工具输出、粘贴附件、截图及临时备份。

| 类型 | 新生成名称示例 | 旧数据兼容 |
| --- | --- | --- |
| 会话 | `20261016_153000_abcd.jsonl`、同 ID 的 `.meta.json` | 原 ID 和文件原名继续读取、恢复、追加；不改写引用 |
| 运行日志 | `desktop_2026_10_16.log`、`tui_input_trace_2026_10_16.log` | 反馈收集同时识别连字符日志与下划线日志，按修改时间选择 |
| 升级日志 | `upgrade_2026_10_16_123.log` | 最近升级日志收集支持两种名称 |
| 反馈包 | `acecode_feedback_<session>_<timestamp>_<platform>.zip` | 内部会话 ID 元数据仍保留原值 |
| 工具与截图 | `bash_<id>.txt`、`mcp_image_0.png`、`simulator_<device>_<timestamp>.jpg` | 历史消息里存储的路径直接按记录读取 |
| 粘贴文本 | `pasted_text_20261016_153000_2.txt` | 旧附件名称、附件 ID 和来源元数据继续识别 |
| 导出 | `acecode_audit_<timestamp>.csv`、`mermaid_flowchart.svg` | 用户最终选择的保存路径不作重写 |
| 配置备份 | `config_backups/last_good/config.json` | 已有 `config-backups/last-good/` 继续使用，不创建空替代存储 |
| 固定运行文件 | `desktop_managed.json`、`agent_browser.json`、`tool_rewrites.json`、`data_dir.redirect.json` | 已有旧路径继续读写 |
| 定时数据 | `scheduled_loops.sqlite3` | 已有 `scheduled-loops.sqlite3` 继续使用原数据库 |
| 开发运行目录 | `.acecode/dev_run/<worktree>_<commit>/` | 旧目录仍可复用；清理仍要求确认 PID 已死亡 |

## 兼容边界

- 不批量重命名旧数据。持久化路径仅在没有旧资源时创建下划线名称；新旧固定资源同时存在时优先旧资源，避免升级意外打开一份空数据。
- 会话的新旧 ID 作为不同身份原样处理，不把连字符替换后的字符串当作旧 ID 的别名。旧 PID 后缀实验文件仍不进入正常列表。
- ISO 时间文本、标准 UUID、API 路由、已发布包名、Git 分支名及主题包等既定协议格式保持其约定。SQLite 自身的 `-wal`、`-shm` 后缀由 SQLite 管理。
- 跨版本进程共用的单实例锁和系统命名管道保留原有协调名称，避免旧新版进程各自取得不同的锁。已有种子资源或用户文件的复制保留原名。
- 本次只调整自动生成的运行数据。用户明确输入的文件路径、导入文件名、会话显示标题，以及 AI 自主创建的工程文件不作强制改写。
- 新版支持读取旧数据；旧版是否支持新版新增数据由旧版实现决定。

## 实现入口

`src/base/utils/generated_file_names.hpp` 提供新名称转换及旧路径选择。会话生成由 `SessionStorage::generate_session_id()` 负责；日志写入与反馈发现分别在 Logger / 输入追踪、feedback 模块内维护。FTXUI 输入追踪名称由 overlay port 固化，并提升 port version，确保重建依赖时使用一致命名。
