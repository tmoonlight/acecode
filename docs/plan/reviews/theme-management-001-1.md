---
task: theme-management-001
scope: ThemeStore 与 ZIP helper 的独立存储审查
date: 2026-09-12
reviewer: theme_package_review
result: pass
validation: static-only
---

# 审查范围与结论

结论：**pass，仅限本阶段存储增量的静态审查**。首轮发现 4 项 blocking 问题，开发者修复后已逐项静态复核关闭。C++ 回归执行、HTTP 与偏好持久化集成、原生选择器及 Web/UI 验证仍待后续，不能据此将整项任务标为已交付。

本次只审查 `src/adapters/themes/theme_store.{cpp,hpp}`、新 `src/adapters/themes/theme_package.{cpp,hpp}` 及相关测试设计。遵循任务授权，未修改实现、未运行共享 C++ 构建或测试、未提交或切换分支。

比较基线为 `build/theme-export-baseline-20260912/` 中的本轮前快照，工作区 HEAD 为 `71e770c2904c3efc8571cb085bf7cf31bf44f75b`。已排除基线中的 AI 主题创建和其它无关脏改动。首轮发现记录使用当时的源码位置；各项“修复复核”使用修复后的源码位置。

修复复核快照的 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| `src/adapters/themes/theme_store.cpp` | `3AA0131BA809C1CA272E790AD9DE7271E9ED53A94F6C6E519618F214C1E91C38` |
| `src/adapters/themes/theme_store.hpp` | `04C3D56F4E9D814215478ED9C6C61BF9D20058B5363D53F92C356D2E7E85C3EA` |
| `src/adapters/themes/theme_package.cpp` | `86FFB87475068CC5EFA1145C8F695F80BA005DCF3F5A30ECBB8CB053AE971775` |
| `src/adapters/themes/theme_package.hpp` | `209FCEE563953F55085EE98D116D3C3F12030F95329BD98BD08F78D2CFC22AAC` |

# Confirmed findings

以下“confirmed”指因果链已由实现和依赖源码静态证实，**不表示已执行 C++ 故障复现**。4 项已全部反馈给主代理，并由开发者修复；回归运行证据待追加。

## F1 — P1 / blocking / 已静态复核关闭：线程注册与满额作业回收存在竞态

- 契约：`openspec/changes/manage-custom-theme-packages/design.md:44-46` 的有界作业与明确并发错误；`specs/custom-theme-management/spec.md:38-40` 的失败与并发行为。
- 首轮代码：`src/adapters/themes/theme_store.cpp:597-613`，先在 `exports_mu_` 内 `emplace`，解锁后才对 `job->worker` 赋 `std::thread`；回收分支在同一互斥量内调用该成员的 `joinable()` / `join()`。
- 影响：快速缓存复用作业可以在启动方完成线程句柄赋值前进入终态。并发第 33 次导出若回收该作业，会与线程句柄赋值发生数据竞争；若先看到未赋值的句柄并移除作业，启动方随后查找 job ID 失败并析构仍可 join 的线程，触发 `std::terminate`。
- 复现思路：保留 31 个处于压缩或保存阶段的作业；对第 32 个快速复用作业，在启动线程的 `std::thread` 临时对象构造后、成员赋值前设调试屏障，让工作线程先到达终态，再发起第 33 个导出触发回收。该屏障用于故障复现，不要求引入产品功能。
- 修复复核：`src/adapters/themes/theme_store.cpp:600-620` 现在在同一 `exports_mu_` 临界区内发布作业并赋好线程句柄，启动失败也在锁内移除；返回时从持有的 `job` 读取状态，避免回收后再次查表。原竞态因果链已消除。

## F2 — P2 / blocking / 已静态复核关闭：列表能观察到未提交的删除隔离

- 契约：`design.md:4` 的同根共享事务边界，以及 `specs/custom-theme-management/spec.md:49-55` 的删除失败保留原状态。
- 首轮代码：`src/adapters/themes/theme_store.cpp:436-459` 的 `local_catalog()` 从枚举根目录开始未持共享根锁；`remove_local()` 先把主题目录移动到 `.deleted-*`，之后才调用可能阻塞或失败的 `commit()`。
- 影响：另一个同根 ThemeStore 在该窗口枚举不到主题 ID，因而完全绕过后续带锁的 `definition()`。即使最终偏好持久化失败并恢复文件，列表请求也可能先返回缺失的主题；只有本地主题且远端不可用时，还可能返回目录不可用错误。
- 复现思路：在删除的 `commit` 回调中设置 barrier 并最终抛出持久化错误；另一实例同时调用 `catalog()`。修复前列表能提前返回缺项；预期是等待删除事务结束，并在回滚后返回恢复的主题。
- 修复复核：`src/adapters/themes/theme_store.cpp:439-462` 现在在目录枚举前持有 `local_state_->mutex`，覆盖整个本地列表快照。相同根实例经 `shared_root_state()` 共用该互斥量。原未提交状态的可见窗口已关闭。
- 基线归因：列表枚举代码原本已存在，但可观察的目录隔离窗口由本轮删除事务新增，本项只针对这条新增交互。

## F3 — P2 / blocking / 已静态复核关闭：缓存 ZIP 比对漏掉 EOF 时执行的 CRC 校验

- 契约：`design.md:8` 的旧包内容校验；`specs/custom-theme-management/spec.md:27-36` 的有效 ZIP 复用和失效包重建。
- 首轮代码：`src/adapters/themes/theme_store.cpp:237-248` 的 `unpack()` 读取恰好 `zip_stat.size` 字节后退出；本轮 `run_export()` 在约 `646-654` 行把 `unpack(cache) == files` 用作缓存有效性的依据。
- 影响：只破坏条目 CRC 字段而不改解压后内容的 ZIP，仍可通过逐文件内容相等检查，被标记 `reused:true` 并导出，标准解压器会报告校验失败。
- 依赖证据：本机 `build/vcpkg_installed/x64-windows-static/include/zipconf.h` 确认为 libzip 1.11.4；对应 `C:/vcpkg/buildtrees/libzip/src/v1.11.4-05742a9dee.clean/lib/zip_source_crc.c:97-118` 在下一次读取到 EOF 时核验 CRC，`zip_fclose.c` 不补做该读取。官方版本源码也给出相同行为：[libzip v1.11.4 CRC source](https://raw.githubusercontent.com/nih-at/libzip/v1.11.4/lib/zip_source_crc.c)。
- 复现思路：保留三个文件的原始内容，同时把一个条目 local header 和 central directory 中的 CRC 改成相同的错误值；执行导出，应判定缓存失效、重建并返回 `reused:false`，重建结果应能由独立 ZIP reader 读至 EOF。
- 修复复核：`src/adapters/themes/theme_store.cpp:248-250` 现在额外读取 1 字节，要求严格得到 EOF；返回数据或 CRC/长度错误都会拒绝该包。新旧缓存路径与新生成包校验均复用该修复。
- 基线归因：旧下载路径已有外部 SHA-256 校验。本轮新增的本地缓存复用缺少该外部可信摘要，因此本次检查必须覆盖完整 ZIP 读取语义。

## F4 — P2 / blocking / 已静态复核关闭：偏好提交后仍可能抛出删除失败

- 契约：`design.md:48` 的文件与偏好事务，以及 `specs/custom-theme-management/spec.md:49-55` 的失败保留原状态。
- 首轮代码：`src/adapters/themes/theme_store.cpp:772-787` 在 `commit()` 成功后、回滚 `try` 范围之外再次 `check_tree(quarantine)`。
- 影响：前面分别允许主题树和包目录最多 4096 个条目；隔离后再次扫描时加上 `theme` / `packages` 包装目录，总数可超过 4096。此时偏好已提交，主题已移走，但方法抛出 `THEME_UNSAFE_PATH`，表现为“删除失败”却没有保留原状态。
- 复现思路：给有效的单版本主题树补普通空文件，使其递归条目数恰为 4096。单树预检通过；隔离后包装层使第二次扫描超限。测试用 `commit_called` 标志或真实偏好文件确认，失败路径必须发生在提交前并恢复原文件，或者提交后只返回成功及清理待处理状态。
- 修复复核：`src/adapters/themes/theme_store.cpp:766-798` 把隔离目录检查放在 `commit()` 之前且包含在回滚 `try` 中。提交之后使用带 `error_code` 的清理；清理失败返回 `deleted:true` 和 `cleanup_pending`，不再混淆已提交结果。原确定性失败已消除。

# 本轮其它已检查路径

- 新包写入 `exports/<id>/<version>.zip`。旧 flat 包只有完整内容与安装快照一致才复用；删除旧包先解析内部 manifest 并匹配 id/version，未按前缀认领。先前 `ai-name / 2-1.0.0` 与 `ai-name-2 / 1.0.0` 的 flat 名称歧义已有隔离方案，仍应保留兼容回归。
- `require_local()` 对 blue/orange/eva-01 的保护，以及本地 ID、安装指针、主题文件、导出目录的静态 symlink/reparse 拒绝路径已检查。
- 同根安装、导出快照和删除共用根锁；按 ID 的导出预留也覆盖原生保存选择器期间。取消选择不会产生导出作业。
- `theme_package.cpp` 的确定进度来自 libzip 回调；回调内异常不会穿过 C ABI。导出采用临时包完成校验后再发布。
- 原生保存先写相邻临时文件，再在取消互斥下替换目的文件；异常清理只删除本作业实际创建的目的临时文件。此项是源码检查，尚不能替代 Windows 原生写入失败和取消的实测。

# Hypotheses 与后续验证边界

## H1 — P3 / non-blocking / 待实测：作业终态与导出预留释放存在短窗口

`run_export()` 先发布 completed/failed/cancelled，再清理临时文件并移除 `active_exports`。当前 `tests/themes/theme_export_test.cpp` 的 `finish()` 只等终态，随后立即调用下一次 `start_export()` 或 `remove_local()`；在特定调度下可能仍收到 `THEME_BUSY`。未运行 C++，也尚未把这项定为违反产品契约；若回归中出现该现象，应核对终态与预留释放的一致性，不能直接归因于测试环境。

本次读取的新 `theme_export_test.cpp` 已设计正常复用、缺包/坏包重建、旧包复用、同根选择器预留、原生保存成功、helper 压缩/取消、删除回滚、内置保护和链接拒绝等用例，但开发者仍在补回归，以下执行证据尚未取得：

- F1-F4 的定向回归，尤其 EOF/CRC 破坏包和同根删除回滚时的列表阻塞。
- flat 包 manifest 属于另一个合法主题时的保留，以及新包目录互不影响。
- ThemeStore 层真实取消到临时文件清理、已有目的文件不变的完整链路；仅 helper 取消或无效目标路径检查不能替代原生保存阶段失败。
- Windows reparse/junction 的测试结果；允许跳过的 symlink 用例不能单独证明该平台已覆盖。

后续应在开发者完成共享构建后复核实际测试结果，再按主代理安排检查 HTTP、偏好持久化、原生类型和已完成的 UI。该文档不评价尚未交接的前端实现。
