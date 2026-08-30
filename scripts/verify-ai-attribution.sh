#!/usr/bin/env bash
#
# 校验 strip-ai-attribution.sh 的结果。
#
# 两件事:
#   A. 署名归零 —— 没有残留的 anthropic / copilot-swe-agent 署名元数据
#   B. 内容零改动 —— 重写前后逐个提交比对 tree hash,任何一条不等即失败
#
# 用法:
#   scripts/verify-ai-attribution.sh <重写后的仓库> [原始仓库]
#
# 只给一个参数时只做 A;给两个参数时 A + B 都做。
#
set -uo pipefail

NEW_REPO="${1:?用法: $0 <重写后的仓库> [原始仓库]}"
OLD_REPO="${2:-}"
fail=0

echo "=========== A. 署名残留检查 ==========="
cd "$NEW_REPO"

n_trailer=$(git log --all --format='%B' | grep -icE 'co-authored-by:.*(noreply@anthropic\.com|copilot.*@users\.noreply\.github\.com)' || true)
n_session=$(git log --all --format='%B' | grep -icE '(claude-session:|https?://claude\.ai/code/session_)' || true)
n_footer=$(git log --all --format='%B' | grep -icE 'generated with \[?claude code' || true)
n_author=$(git log --all --format='%an <%ae>%n%cn <%ce>' | grep -icE 'noreply@anthropic\.com|copilot-swe-agent' || true)

printf '  Co-Authored-By (AI)      : %s\n' "$n_trailer"
printf '  Claude-Session / 会话回链 : %s\n' "$n_session"
printf '  Generated with 页脚       : %s\n' "$n_footer"
printf '  作者/提交者身份            : %s\n' "$n_author"

total=$((n_trailer + n_session + n_footer + n_author))
if [ "$total" -ne 0 ]; then
  echo "  => 失败: 仍有 $total 处残留"
  fail=1
else
  echo "  => 通过: 零残留"
fi

if [ -z "$OLD_REPO" ]; then
  exit $fail
fi

echo
echo "=========== B. 内容零改动检查 ==========="
# 按「提交序号」对齐两侧历史(重写只改元数据,提交条数与拓扑顺序不变),
# 逐条比对 tree hash。tree hash 相同 == 该提交的整棵文件树逐字节相同。
git -C "$OLD_REPO" rev-list --reverse --topo-order --all --format='%T' \
  | grep -v '^commit ' > /tmp/_old_trees.txt
git -C "$NEW_REPO" rev-list --reverse --topo-order --all --format='%T' \
  | grep -v '^commit ' > /tmp/_new_trees.txt

old_n=$(wc -l < /tmp/_old_trees.txt)
new_n=$(wc -l < /tmp/_new_trees.txt)
printf '  提交数  旧=%s  新=%s\n' "$old_n" "$new_n"
[ "$old_n" = "$new_n" ] || { echo "  => 失败: 提交数不一致"; fail=1; }

if diff -q /tmp/_old_trees.txt /tmp/_new_trees.txt >/dev/null; then
  echo "  => 通过: $new_n 个提交的 tree hash 全部一致,文件内容零改动"
else
  echo "  => 失败: $(diff /tmp/_old_trees.txt /tmp/_new_trees.txt | grep -c '^<') 个提交的 tree 发生变化"
  fail=1
fi

echo
echo "=========== C. 标签与分支 ==========="
printf '  tag   旧=%s  新=%s\n' "$(git -C "$OLD_REPO" tag | wc -l)" "$(git -C "$NEW_REPO" tag | wc -l)"
printf '  分支  旧=%s  新=%s\n' \
  "$(git -C "$OLD_REPO" for-each-ref --format='%(refname)' refs/heads | wc -l)" \
  "$(git -C "$NEW_REPO" for-each-ref --format='%(refname)' refs/heads | wc -l)"

exit $fail
