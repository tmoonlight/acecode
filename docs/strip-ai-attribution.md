# 剔除 AI 署名

本仓库的著作权归作者所有,提交历史不带第三方 AI 署名。本文档记录现状、清除办法与防复发措施。

## 一、署名分布(2026-08-29 全量统计,816 个提交)

历史里的署名有 **4 种形态**,分布在 33 个分支、72 个 tag 上:

| 形态 | 数量 | 示例 |
|---|---|---|
| `Co-Authored-By:` trailer 指向 anthropic | 107 个提交 | `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` |
| `Claude-Session:` 会话回链 | 16 个提交 | `Claude-Session: https://claude.ai/code/session_...` |
| 作者/提交者身份 = Claude | 27 作者 / 26 提交者 | `Claude <noreply@anthropic.com>` |
| 作者身份 = Copilot bot | 8 个提交 | `copilot-swe-agent[bot] <198982749+Copilot@users.noreply.github.com>` |

Claude 署名的型号名有 6 种变体(`Claude`、`Claude Opus 4.7 (1M context)`、`Claude Opus 4.8`、
`Claude Opus 4.8 (1M context)`、`Claude Opus 5`、`Claude Fable 5`),按邮箱域匹配可一次覆盖全部。

`Co-authored-by: Copilot <...>` 形态本仓库没有出现过,清除脚本仍然覆盖它以防将来混入。

### 不属于署名、必须保留的东西

这三类内容都会命中 `claude` / `copilot` / `anthropic` 关键词,但它们是**内容不是署名**,一刀切会误伤:

1. **功能描述** —— `feat: Implement GitHub Copilot authentication and model management`、
   `feat(headless): 移植 claude -p 无头打印模式`、`feat: 复刻 Claude Code 的 worktree 隔离功能`、
   `Add AnthropicProvider implementation`。本项目本来就实现了 Copilot / Anthropic provider。
2. **分支名** —— `Merge pull request #20 from claude/acecode-cache-hit-optimization-lx9hxc`。
   那是分支名,不是署名。
3. **人类协作者的 co-author** —— `shaohaozhi286`、`huangxiangxin` 共 9 条,必须原样保留。

清除脚本按**邮箱域**而不是关键词判定,天然避开这三类。

## 二、清除办法

### 前置

```bash
pip install git-filter-repo
```

### 步骤

在一份 **mirror 克隆**上操作。mirror 会把远端全部 33 个分支和 72 个 tag 一次性拿全并一起重写;
在普通克隆上跑只会重写当前分支,其余分支下次推送时会把旧 SHA 又带回来。

```bash
# 1. 完整镜像(不是浅克隆)
git clone --mirror https://github.com/tmoonlight/acecode acecode-mirror.git

# 2. 留一份重写前的快照,供内容比对
cp -r acecode-mirror.git acecode-before.git

# 3. 重写(第 2、3 参数是署名要归属的人)
scripts/strip-ai-attribution.sh acecode-mirror.git "tmoonlight" "shaohaozhi@live.cn"

# 4. 校验:署名归零 + 内容零改动
scripts/verify-ai-attribution.sh acecode-mirror.git acecode-before.git

# 5. 校验全绿之后再推(这一步不可逆,见下方风险)
cd acecode-mirror.git
git remote add origin https://github.com/tmoonlight/acecode
git push --mirror --force origin
```

`verify` 脚本做两件事:

- **A. 署名归零** —— 4 类署名的残留计数必须全为 0。
- **B. 内容零改动** —— 逐个提交比对 tree hash。tree hash 相同即该提交的整棵文件树逐字节相同,
  证明只动了元数据。816 条全部一致才算通过。

实测结果(2026-08-29):A 四项全 0;B 816/816 tree hash 一致;分支 33→33,tag 72→72;
无提交信息被清空;9 条人类 co-author 全部保留。

## 三、推送前必须知道的风险

重写会改变**每一个提交的 SHA**。这一步推上去之后:

- **所有已有克隆都会对不上。** 协作者必须重新克隆,或 `git fetch && git reset --hard origin/master`;
  在旧历史上 rebase 会把旧提交整批带回来。
- **72 个 tag 全部指向新 SHA。** GitHub Release 关联的是 tag 名,tag 会被一起重写并强推,
  Release 本身不受影响,但 Release 页面上"该 tag 对应的提交"链接会指向新 SHA。
- **已合并 PR 里的提交链接会失效**(指向不再存在的 SHA)。PR 本身与讨论不受影响。
- **旧 SHA 在 GitHub 上仍可能通过直链访问一段时间**,直到 GitHub 侧 gc。
  彻底清除需要联系 GitHub Support。
- **11 个未合并的远端分支**也会被一起重写(mirror 方式已覆盖),它们上面的 19 个提交同样换 SHA。

建议在推之前先把当前远端状态备份一份:

```bash
git clone --mirror https://github.com/tmoonlight/acecode acecode-backup-$(date +%Y%m%d).git
```

## 四、防复发

已落地三道:

1. **`.claude/settings.json` 的 `includeCoAuthoredBy: false`** —— 关掉 Claude Code 的
   `Co-Authored-By` trailer 与生成页脚。作用范围是本仓库。
2. **`.githooks/commit-msg`** —— 对**所有**工具生效的兜底闸。本仓库被多个 AI 工具改过
   (`claude/*`、`codex/*`、`copilot/*` 分支都存在),每个工具的开关各自独立,漏配一个就又混进署名;
   hook 是唯一一处覆盖全部来源的地方。每个克隆启用一次:

   ```bash
   git config core.hooksPath .githooks
   ```

3. **清掉文档里的署名样例** —— `docs/superpowers/plans/2026-05-09-model-selection.md` 里
   12 处 `git commit -m "..."` 示例带着 `Co-Authored-By` 行,照抄就会重新引入,已移除。

hook 与脚本共用同一套判定规则(按邮箱域,而非关键词),两者行为一致。
