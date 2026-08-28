# 复盘:AI 生成的文件链接点不开

用户反馈两种表现:点正文里的文件链接**毫无反应**;或者点完前端**像崩溃重启一样跳回首页**,会话、滚动位置、输入框全部丢失。

复现会话 `20260828-111940-c783`(no-workspace),模型生成 xlsx 后输出 `[下载随机销售数据.xlsx](随机销售数据.xlsx)`。

排查下来是**四个互相独立的缺陷**,分布在渲染、编码、路径、数据传递四层。任何一个单独存在都足以让链接失效,叠在一起时症状还会互相掩盖 —— 这也是它拖了这么久才被定位的原因。

---

## 缺陷一:相对 href + SPA fallback = 静默整页重载

`[文本](随机销售数据.xlsx)` 渲染出的 `<a href="随机销售数据.xlsx">` 是**相对 URL**。点击后浏览器导航到 `http://<daemon>/随机销售数据.xlsx`,而 daemon 的静态资源兜底(`routes_misc.cpp::serve_path`)对任何未知路径都返回 **200 + index.html**。

于是:整页重载 → React 树重建 → 回到首页。**浏览器不报任何错误**,因为服务端确实回了 200。

`Message.jsx` 的 assistant 气泡有点击拦截会 `preventDefault`,但 transcript 里还有别的 markdown 渲染位置没有各自的拦截器:

- `ToolBlock.jsx` 的 `task_complete` 完成总结
- `ChatView.jsx` 的会话摘要
- `SidePanel` 的文件预览、`SideQuestionCard`

**修法**:不再依赖"每个调用点记得 preventDefault"。`markdown.js` 的 `link_open` 对本地文件链接**直接删掉 href**,真实路径只留在 `data-file-path`。没有 href 的 `<a>` 浏览器不可能导航,最坏情况只是点了没反应。去掉 href 会连带丢失原生可聚焦性,补 `role="link"` + `tabindex="0"`。

> **教训**:能在渲染层结构性排除的故障,不要交给 N 个调用点的自觉。新增一个渲染位置的人不会知道有这条约定。

---

## 缺陷二:markdown-it 的百分号编码泄漏进文件路径

markdown-it 在 parse 阶段就对 href 做 `mdurl.encode`。`随机销售数据.xlsx` 变成 `%E9%9A%8F%E6%9C%BA...`,这个编码值被原样写进 `data-file-path`,前端拼 API URL 时**再编码一次**,服务端收到 `%E9%9A%8F` 字面量 → 找不到文件。

**任何非 ASCII 文件名的链接都打不开**,而英文文件名一切正常 —— 这个差异让问题长期被当成偶发。

附带伤害:反斜杠被编成 `%5C` 后,`N:\Users\x.md` 漏过 `WIN_ABS` 盘符判定,`N:` 被 `URL_SCHEME` 当成危险协议,链接直接被剥成纯文本。

**修法**:`classifyFileLink` 入口统一解码。放在函数内部而不是各调用点,是为了让 `validateLink` 与 `link_open` 继续共用同一套判据(该文件头部记录的单一事实源约定)。顺带把 `javascript%3Aalert(1)` 这类编码绕过也还原成可识别的危险 scheme,安全性不降反升。

---

## 缺陷三:`cwd` 语义重载,兜底指向了 daemon 自己的目录

后端对 no-workspace 会话**刻意**把 `cwd` 清成空串(5 个响应出口),因为前端把 `cwd` 当作"属于哪个 workspace"的归属字段。

前端算文件预览根目录时:

```js
sessionWorkingCwd({ cwd: ref?.cwd || '', fallbackCwd: health?.cwd || '' })
```

`ref.cwd` 为空 → 回退到 `health.cwd` = **daemon 进程自己的工作目录**。实测 `health.cwd = N:\Users\shao\se`,于是预览跑去 `N:\Users\shao\se\随机学生成绩单.xlsx` 找文件,报「文件不存在」。

这个兜底最坑的地方在于**它不沉默**。它给出一个看起来煞有介事的错误路径,把"预览配置错了"伪装成"文件丢了" —— 用户因此以为是文件写错了位置,排查方向直接被带偏(实际文件一直好好躺在会话目录里)。

**修法**:

- 后端新增 `working_cwd`:会话真实工作目录,与 workspace 归属无关,no-workspace 会话照样给。`cwd` 的既有语义一个字没动。
- 前端反转判断顺序:**先解析目录,再由目录决定可用性**,而不是按 no-workspace 一票否决。
- no-workspace 会话**不再回退到 daemon cwd**。宁可算不出根目录(预览不打开),也不指向一个无关目录。
- `allowed_file_cwds()` 纳入活跃会话自身 cwd —— 会话的工具本来就在那个目录读写,文件树没理由列不出来。

> **教训**:错误的兜底比没有兜底更糟。一个"总能返回点什么"的 fallback,会把配置错误伪装成数据缺失,让排查从第一步就走错方向。兜底的正确姿势是**语义相关**:daemon 的 cwd 与一个 no-workspace 会话毫无关系,它就不该出现在这条链路里。

---

## 缺陷四:显式字段白名单漏字段,两条入口行为相反

修完前三个之后,`?open=<session-id>` 入口验证通过,但用户从**侧边栏**点进会话仍然打不开。

分叉点在 `App.jsx::resumeAndOpenSession`:

```js
const shouldResume = !readOnly && (options.forceResume || target?.active !== true);
```

**已经 active 的会话不会重新 resume**,`resumed = {}`。ref 只能从侧边栏传来的 target 取值 —— 而 `Sidebar.jsx::sidebarSessionTarget` 是一个**显式字段白名单**,`working_cwd` 不在里面,被整个丢掉了。

`?open=` 入口因为 target 没有 `active` 标记反而会走 resume,从响应里拿到了 `working_cwd`。**同一个功能在两条入口下表现相反**,而验证时恰好只覆盖了能用的那条。

同类白名单共三处,一并补齐:`sidebarSessionTarget`、`newSession.js::sessionRefFromCreateResponse`、`gridPinnedSessions.js`。其中 `newSession.js` 还带着与缺陷三同源的 `health.cwd` 兜底。

> **教训一**:显式字段白名单是静默失效的温床。漏一个字段不报错、不崩溃,只让某条入口进来的功能悄悄失效。
>
> **教训二**:一个功能有多条入口时,只验证一条等于没验证 —— 尤其当入口之间存在"是否触发某个副作用"(这里是 resume)这种隐式分支。

---

## 回归防线

| 测试 | 守住什么 |
|---|---|
| `fileLink.test.js` | 本地文件链接不得带 href;去 href 后仍可键盘触达;外链与 thread 链接必须保留 href |
| `sessionJump.test.js` | no-workspace 会话 ref 携带 workingCwd;预览根目录不得取 daemon cwd(含缺 working_cwd 时宁可为空) |
| `previewRootArchitecture.test.js` | **扫描全部四个会话 ref 构造入口**,任一漏掉 `workingCwd` 即失败 |

最后一条是针对缺陷四的针对性防线:白名单遗漏无法靠功能测试可靠发现(会像这次一样只覆盖到能用的那条路径),只能用源码级断言把"所有入口"这个集合本身钉住。

---

## 排查方法上的收获

1. **两个症状不一定是一个 bug。** "点了没反应"和"跳回首页"被当成同一个问题描述,实际分属缺陷三和缺陷一,根因毫无关系。
2. **先确认事实再推断。** 用户报"文件写到了 `N:/Users/shao/se/`",实际 `ls` 一下文件就在会话目录里 —— 那个路径是**预览解析出来的**,不是文件真实位置。一次 `curl /api/health` 就锁定了 `health.cwd` 这个来源。
3. **200 不等于正确。** SPA fallback 让"导航到不存在的路径"表现为一次成功的页面加载,浏览器控制台干干净净。
