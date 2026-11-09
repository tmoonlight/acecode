# 后台任务(后台命令 / 后台子代理)

让模型放心把长任务交出去:命令或子代理在后台跑,结束时框架自动把结果送回会话;
会话在跑就在下一个模型边界插入,空闲就开一个新回合。做法参照 Claude Code 与
opencode(两者源码已核对),要点是「框架兑现叫醒」,而不是靠模型轮询。

## 组成

| 位置 | 职责 |
|---|---|
| `src/adapters/tool/shell_process.{hpp,cpp}` | 一条 shell 子进程的启动(沙箱令牌 / bwrap / Seatbelt)、非阻塞读、退出检查、杀进程树。从 bash 工具拆出,好让跑到一半的命令整体交给后台 |
| `src/adapters/tool/background_tasks.{hpp,cpp}` | 会话级登记表:一个泵线程轮询本会话全部后台任务,输出写 `<scratch>/background/<id>.log`;结束 / 新输出(monitor)/ 长时间无输出时生成通知;关闭时杀掉还在跑的命令 |
| `src/engine/agent/background/agent_background.cpp` | AgentLoop 持有登记表;通知经邮箱(与网状蜂群信封同一个)投递,`trigger_turn` 唤醒空闲会话;状态变化发 `background_task` 事件与 TUI 回调;`request_background()` 递增「转后台」信号 |
| `src/adapters/tool/bash_tool.cpp` | `run_in_background` / `monitor` 参数;前台超时**转后台而不是杀掉**;收到「转后台」信号时把进程连同已读输出交给登记表;开头是长 sleep(≥25 秒)的命令直接拒绝 |
| `src/adapters/tool/stop_background_task_tool.cpp` | 模型停止后台任务;不带 id 时列出运行中的 |
| `src/host/session_host/tools/spawn_subagent_tool.cpp` | `wait=false` 交给父会话登记表观察,结束时通知父会话;`wait=true` / `wait_subagent` 收到「转后台」信号时停止阻塞;子会话自己的后台任务没完不算做完 |
| `src/apps/web/routes/routes_background_tasks.cpp` | 列表、输出末尾、停止、转后台四个接口(见 docs/daemon-api.md「Background tasks」) |

## 几条约定

- **通知是内部上下文,不是用户发言**:落盘为 user 角色 + `metadata.background_task`。
  `llm/message_predicates.hpp` 的 `is_background_task_message` 与网状蜂群的非 NEW_TASK
  信封同等对待:不计回合、不当标题 / 摘要、不触发 UserPromptSubmit、不建检查点、
  不进分叉历史;Web 转成系统提示行,TUI 显示成系统行。Web 折叠回合时,前一回合
  已收尾(task_complete 或助手正文)的通知行开始新回合,前一回合保留自己的总结;
  回合中途插入的通知不切分回合(`transcriptProjection.js` 的 `turnHasEnded`)。
- **转后台的三个时机**:前台命令超时;用户插话(`steer_input` 递增信号,让消息能
  马上送到模型);用户手动(Web 活动行「转到后台」、TUI Ctrl+B、`POST /background`)。
  Esc 中断回合仍然杀前台命令,不影响已在后台的任务。
- **完成判定**:登记表在通知交给 Sink 之前仍把任务算作活跃(`running_count`),
  headless 与子代理完成判定据此不会在「任务结束」与「通知进邮箱、开回合」之间
  的空隙里误判为空闲。
- **Web 呈现**:详情栏「后台任务」页签(每会话一个,后台命令与子代理合在一个列表)。摘要面板的
  「后台任务」分组常驻,列运行中的任务和耗时,末尾一行「N 已完成」,点它打开页签。子代理列表里
  `created_at` 为空时(子会话刚建好还没写元数据)按第一次看到的时间计时
  (`subagentTasks.js` 的 `mergeSubagentTaskList`)。
- **静默提醒**:后台命令持续无输出 20 分钟提醒一次模型,之后间隔翻倍。
- **生命周期**:后台任务属于会话的 AgentLoop;会话销毁、daemon 退出、TUI 退出时
  杀掉还在跑的命令。daemon 重启后不恢复。
- **headless**:回合结束后若还有后台任务、待投递的通知或被唤醒的回合,等全部结束
  (连续两次确认空闲)再输出最后一条回复并退出。

## 测试

- C++:`tests/tool/background_tasks_test.cpp`(登记表、bash 后台化、长 sleep 拦截)。
- Web:`web/src/lib/backgroundTasks.test.js`(命令任务状态、通知消息呈现、转后台按钮判定)。
