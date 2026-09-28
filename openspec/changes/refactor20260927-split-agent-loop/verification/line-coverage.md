# 拆分行追溯

源提交: c3348df148d51ff799a68b17ce6af0899b5ce2fc。该提交已包含 P0/P2 前置整理,也是本次 master 实施的起点。

[line-coverage.tsv](line-coverage.tsv) 覆盖原始文件全部 7949 行,无遗漏、无重复;其中 3596 行为逐字节相同的 copy,其余为 edited。edited 的 reason 说明职责归属和签名、成员访问、装配或所有权调整;目标范围指向当前实现。追溯表用于审查去向,不能单独证明行为等价。行为证据另见表征测试、全量回归和 [Windows 验证记录](../../refactor20260927-restructure-src-layers/verification/windows-phase1-validation.md)。

核查使用 scripts/refactor/check_line_coverage.py,指定上述 source-ref、此 map 和原始文件。该工具对 copy 检查内容完全一致,对 edited 检查说明及目标范围存在,对每个源文件检查覆盖完整且唯一。两份检查输出均保存在总验证目录。
