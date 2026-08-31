#pragma once

// PA 适配层的总开关。目录定位与收录标准见 src/pa/README.md。
//
// 单独成文件是为了让开关覆盖整个适配层:判定(pa_quirks)与自适应预算
// (pa_context_budget)必须同时受控,否则「关掉验证一下」只关了一半,得到的
// 结论是错的。

namespace acecode::pa {

// 默认启用:收录的每一条都是「通用路径认不出、补上才能正常恢复」的判定,
// 或「撞过墙才生效」的观测,对非 PA 环境不产生行为差异。留出开关是为了在
// 上游修好之后能一键验证「关掉是不是也正常」,以及在适配本身出问题时止血。
bool enabled();
void set_enabled(bool value);

} // namespace acecode::pa
