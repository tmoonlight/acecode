# B-11 新回合重置逐字段对照

来源:7942011b:src/main.cpp,当前提取前 main/composer 实现;未改变任何调用位置、锁区间或前后副作用。以下为源码对照,运行验证集中在一期末尾。

| 调用点 | phrase | 开始时间/两种 token 计数 | is_waiting | TurnObservation | 气泡/跟尾/MCP 等待 |
| --- | --- | --- | --- | --- | --- |
| Copilot 登录 | 随机 | now / 0 / 0 | true | 不动 | 原位置显示认证消息,不等待 MCP |
| 初版 busy=true 且原先空闲 | 随机 | now / 0 / 0 | helper 保留,随后赋 busy | 不动 | 不动 |
| 最终版 busy=true 且原先空闲 | 随机 | now / 0 / 0 | helper 保留,随后赋 busy | helper 前清空两字段 | 不动 |
| pending queue 出队 | 随机 | now / 0 / 0 | true | 不动 | 先加气泡/按拖动状态跟尾/clamp,后锁外等待 MCP |
| 远程 IM 入站且空闲 | 随机 | now / 0 / 0 | true | 不动 | 先气泡/跟尾/clamp,后锁外等待 MCP |
| Enter 普通提交且空闲 | 随机 | now / 0 / 0 | true | 不动 | MCP 等待在重置前,原来的气泡/跟尾/clamp 不动 |
| Enter Shell | Running shell | now / 0 / 0 | true | 不动 | 不等 MCP,仍走 submit_shell |

begin_user_turn_locked 只合并 phrase、开始时间、streaming_output_chars、turn_completion_tokens_confirmed、可选的 is_waiting 五字段。WaitingUpdate::Preserve 保持 busy 回调原来的读旧值/赋 busy 顺序;Shell 用独立 phrase 参数。观察字段、拖动跟尾、历史/输入、MCP 等待、通知、取消标记均留在各自原位置。没有统一原有不一致的行为。
