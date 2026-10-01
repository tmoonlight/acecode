# 隔离基准

所有数据由仓库脚本合成。输出目录必须不存在，脚本不会覆盖现有用户目录。测量脚本只启动自己的 daemon，使用独立端口、运行目录和进程级 HOME/USERPROFILE/APPDATA；退出时仅结束自己创建的进程。

```powershell
python scripts/bench/session_loading/generate.py --profile <新的临时目录> --sessions 1500 --large-mib 100 20
python scripts/bench/session_loading/inspect_data.py --profile <临时目录>
python scripts/bench/session_loading/measure.py --executable <当前构建的acecode.exe> --profile <临时目录> --output <临时目录>/http.json
python scripts/bench/session_loading/measure.py --executable <当前构建的acecode.exe> --profile <临时目录> --output <临时目录>/browser.json --browser
```

浏览器测量需要 Python Playwright 与 Chromium。可通过 `--static-dir web/dist` 验证最新构建前端；省略时使用二进制内嵌资源。脚本等待侧栏会话行可见后清空 resource timing，记录点击时刻，等待第一条 `[data-chat-row="true"]` 可见，随后观察 30 秒。它只点击会话行，不发送模型请求。假模型端点指向本机关闭端口。

手动复测时，可在 DevTools 中注册以下观察器，在点击前记录 `sessionBenchStart`，首条消息可见后读取耗时和资源：

```javascript
window.sessionBenchLongTasks = [];
new PerformanceObserver(list => sessionBenchLongTasks.push(
  ...list.getEntries().map(e => ({start: e.startTime, ms: e.duration}))
)).observe({type: 'longtask', buffered: true});
performance.clearResourceTimings();
window.sessionBenchStart = performance.now();
// 点击会话，首条消息可见后执行：
const history = performance.getEntriesByType('resource')
  .filter(e => new URL(e.name).pathname.endsWith('/messages'));
console.table({
  first_content_ms: performance.now() - sessionBenchStart,
  history_requests: history.length,
  history_bytes: history.reduce((n, e) => n + e.decodedBodySize, 0),
});
```

时延为本机热文件缓存实测，不是通用性能承诺。首次目录枚举与后续访问分别记录，不能把首次全量列表与后续热列表直接作为同一条件下的加速比。比较各阶段时保持合成数据参数和二进制构建类型一致。原始测量文件仅包含合成标识、请求路径、计时和字节数；认证令牌不写入结果。

## 完成记录

最终验收见 [final-validation.md](final-validation.md)，阶段 2 数值与后续门槛评估见 [stage2-results.md](stage2-results.md)。浏览器可加 `--session-index 1` 测量 20MB 会话，`--check-paging` 记录翻页锚点；`check_navigation.py` 验证字节位置与旧序号链接。
