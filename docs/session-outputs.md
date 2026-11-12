# 会话产物(摘要面板的「产物」分组)

列出这次对话生成、给用户看的文件:报告、表格、图片、音视频、压缩包。代码改动不算
产物,归「变更」。做法参照 Claude 桌面版的 Outputs 面板(界面包源码已核对):列表
不是另存的状态,而是从会话记录推导出来的。Claude 只认 Write 工具和显式交付,这里
多做了一步:bash 跑脚本生成的文件尽力找出来。

## 组成

| 位置 | 职责 |
|---|---|
| `src/domain/session/session_outputs.{hpp,cpp}` | 纯逻辑:从一条消息的元数据取候选、判扩展名、JSONL 行预筛、过滤去重 |
| `src/domain/session/session_outputs_index.{hpp,cpp}` | 读盘:增量扫描会话记录、跟到子代理会话、归一路径、核对文件还在不在、按修改时间排序 |
| `src/adapters/tool/shell_output_files.{hpp,cpp}` | bash 命令结束后找它生成的文件(下面「bash 探测」) |
| `src/adapters/tool/bash_tool.cpp` / `background_tasks.cpp` | 前台命令结束时、后台命令完成通知发出前各调一次探测,结果记进元数据 |
| `src/apps/web/routes/routes_session_outputs.cpp` | `GET /api/sessions/:id/outputs`(见 docs/daemon-api.md「Session outputs」) |
| `web/src/lib/sessionOutputs.js` | 前端纯整形:收起显示前 5 个、重名文件带所在目录、轮询结果没变不重渲染 |
| `web/src/components/SummaryPanel.jsx` | 「产物」分组:点一行在右侧预览打开,右键是「打开文件夹 / 复制文件路径」 |

## 来源

全部取自会话记录里已有的工具结果元数据,**不改写文件的工具**,所以功能上线前的旧
会话也有产物列表:

| 工具 | 记录里的依据 | 算不算 |
|---|---|---|
| `file_write` | `metadata.tool_summary` 的动词 `Created` / `Wrote`,`object` 是路径 | 新建、覆写都算 |
| `apply_patch` | `metadata.files` 里 `type == "add"` 的条目 | 只算新建 |
| `file_edit`、`apply_patch` 的 update / move | — | 不算,相当于编辑 |
| `bash` | `metadata.output_files`;后台命令在完成通知的 `metadata.background_task.output_files` | 探测到的都算,命令失败也算 |
| 子代理 | 派生结果的 `metadata.subagent_session_id` | 跟到子会话的记录,它的产物并进主会话 |

## 过滤

- 路径里有 `.acecode` / `.git` 这一级的不算。会话文件夹自身那一段除外:worktree 会话
  的工作目录就在 `.acecode/worktrees` 下面,只按「路径里有 .acecode」判会让它的列表
  永远是空的。
- 会话文件夹(工作目录、worktree、附加文件夹)内:只留文档类扩展名(md、txt、pdf、
  docx、xlsx、pptx、csv、html、图片、音视频、zip 等,清单在 `session_outputs.cpp`)。
- 会话文件夹外:系统临时目录里的不算,其余任何扩展名都算。
- 已经不在磁盘上的不返回;同一路径只出现一次;按文件修改时间新 → 旧,最多 50 个。

路径比较前两侧都过 `weakly_canonical`(用户目录是目录联接时盘符会不同)。交给界面
的路径再用会话文件夹的原始写法拼回去:界面的预览按它自己知道的工作目录定位文件。

## bash 探测

脚本写文件不经过文件工具,只能事后找。两条线索,都只认文档类扩展名,并且要求文件
修改时间不早于命令开始(放宽 2 秒):

1. **提到的路径**:命令行里的单词(`-o report.pdf`、`--out=a.xlsx`、重定向目标)和
   输出末尾 32 KB 里的路径片段(`Saved to D:\x\a.xlsx`,认中文全角标点;路径带空格
   时从同一行左边的绝对路径起点取整段)。工作目录外的文件只能靠这条。网址、UNC
   路径、带 `~` `$` `%` 的不查。
2. **扫工作目录**:广度优先,点开头的目录和 `node_modules`、`__pycache__`、`venv`
   整个跳过;`build` / `dist` / `out` 这类构建目录只看第一层;不跟符号链接和目录联接;
   上限 6000 个目录项、120 毫秒、8 层。

不找的情况:

- 只读命令(命令分类器判 KnownSafe),所以 `ls`、`cat`、`git status` 没有额外开销。
- 整条命令只有构建 / 包管理 / 版本控制工具(`git pull`、`pnpm build`、`cmake --build`):
  它们一次改动一批文档类文件,不是产物。`uv run`、`cargo run` 这类是在跑用户的
  程序,照常找;带输出重定向的命令一律找。
- 一条命令改动的文档类文件超过 12 个:当作批量操作,扫描结果整个作废,明确提到的
  路径仍保留。

这是尽力而为,会漏:脚本悄悄写到工作目录外、写进被跳过的目录、只是复制了一个旧
文件(修改时间没变)、一次生成十几个以上的文件。也会多:命令运行期间别的会话或
用户自己改了工作目录里的文档,会被算到这条命令头上。

## 界面刷新

界面收不到「bash 写了文件」的事件。摘要面板可见时:打开 / 换会话 / 回合开始和结束
各取一次;回合进行中每 6 秒取一次。服务端每个记录文件只在第一次整读,之后只读新
追加的部分,行先过子串预筛,绝大多数行不解析 JSON。
