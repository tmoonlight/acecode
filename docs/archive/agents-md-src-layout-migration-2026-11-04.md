# AGENTS.md「src 分层搬迁交付记录」归档(2026-11-04 精简前全文)

本文件是根目录 [AGENTS.md](../../AGENTS.md)「src 分层搬迁交付记录」一节在 2026-11-04 精简之前的完整内容:2026-09-28 至 09-29 src 分层搬迁一期的逐日公告与用户决定,原样保留、不再更新,正文里的相对链接按仓库根目录书写。搬迁的当前状态与仍在生效的约定以根目录 AGENTS.md 该节为准,各 change 的实现与验收状态看 `openspec/changes/refactor20260927-*`。

---

## src 分层搬迁交付记录

2026-09-29 用户最新决定:取消等待 06:00 候选窗口,一期现已完成约定的 Windows 验收,立即提交并 push。原公告中的最早开始时间由本条覆盖;通过受保护 master 所需的 PR 完成交付,九个旧 ref/worktree 与已批准的补验安排保持。

2026-09-28 公告:按 [重构设计 D3 / §8.3](openspec/changes/refactor20260927-restructure-src-layers/design.md),P3-02 候选冻结窗口为 **2026-09-29 06:00–18:00(Asia/Taipei,UTC+08:00)**。最早开始时间保留至少一天的公告期;只有 P2 全部验收、P3-01 演练通过后才进入窗口,未满足时顺延并更新本公告。

2026-09-29 已完成主线搬迁并解除冻结:[PR #87](https://github.com/tmoonlight/acecode/pull/87) 已合入 master,post-src-layout 指向 1483069f2b97cde11953da1e5e15c8601e71f464。pre-src-layout、机械提交 blame 登记和 Windows 验收记录均已完成。九个旧 ref/worktree 保留原状,迁移另行安排;不得将保留误记为弃用。

2026-09-28 D27(用户最新指令,优先于 D26 和旧的逐任务流程):剩余一期全部在当前 master 检出实施,先完成全部实现,最后集中做 Windows 本机全量验收,完成后一起提交并 push;中途不新建任务分支/worktree、不逐任务提交/推送、不跑跨端 CI。过程中只做必要的编辑/迁移一致性核对;实施状态与验收状态分别记录。跨端补验暂留后续安排,一期实现范围与 D6–D9 行为约定保持。详情见 restructure-src-layers/design.md D27。

2026-09-29 用户补充决定:P3-03 的九个旧分支保留原状,迁移另行安排;不推进或删除原 ref/worktree,不标记弃用,不阻塞本次主线一期交付。

2026-09-29 验收范围补充(用户确认):人工专项后补,本次按 Windows 自动化及已完成实测交付;未执行的人工项目继续记录,不宣称通过,不阻塞本次主线交付。

2026-09-29 交付收尾最新指令(用户确认):发完后确保 CI/CD 已启动即关机,结果明天查看。正常触发 test,在最终 master 上启动 refactor-matrix(全量及 Deepin)和 package 分支构建验证;此前暂停跨端 CI 的安排由本条覆盖。人工专项与旧分支迁移仍按已批准范围后补。

2026-09-28 D26:一期剩余任务的验收只做 Windows 本机(design.md §7.4 轻量协议:静态闸门 + 复用目录的 Ninja 增量构建 + 用例清单 / target 快照对照,内容改动再跑 `python scripts/refactor/run_fast_tests.py --profile fast`),不 dispatch refactor-matrix、不等 test.yml;macOS / Linux / Deepin 与 package.yml 推迟到 tasks.md 5.4「多平台补验」一次做完。
