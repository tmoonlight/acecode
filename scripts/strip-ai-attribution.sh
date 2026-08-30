#!/usr/bin/env bash
#
# 剔除 git 历史中 Claude / GitHub Copilot 的署名元数据。
#
# 只动署名,不动内容:
#   1. Co-Authored-By: ... <noreply@anthropic.com>        (trailer,全部型号变体)
#   2. Claude-Session: https://claude.ai/code/...         (trailer)
#   3. Generated with [Claude Code] / 机器人生成页脚
#   4. 作者/提交者 Claude <noreply@anthropic.com>         -> 归属人
#   5. 作者/提交者 copilot-swe-agent[bot]                 -> 归属人
#
# 明确不动(这些是内容,不是署名):
#   - 提交信息里对 Copilot / Anthropic provider、复刻 Claude Code 功能的正常描述
#   - "Merge pull request #N from claude/xxx" 里的分支名
#   - 任何文件内容(verify 步骤逐个比对 tree hash 保证这一点)
#
# 用法:
#   scripts/strip-ai-attribution.sh <仓库路径> ["姓名" "邮箱"]
#
# 前置: pip install git-filter-repo
#
set -euo pipefail

REPO="${1:?用法: $0 <仓库路径> [姓名 邮箱]}"
NEW_NAME="${2:-tmoonlight}"
NEW_EMAIL="${3:-shaohaozhi@live.cn}"

command -v git-filter-repo >/dev/null || {
  echo "缺少 git-filter-repo,请先: pip install git-filter-repo" >&2
  exit 1
}

cd "$REPO"

# 浅克隆会把边界外的提交永久丢掉,必须先补全历史
if [ "$(git rev-parse --is-shallow-repository)" = "true" ]; then
  echo "错误: 这是浅克隆(shallow)。先执行 git fetch --unshallow,否则重写会丢历史。" >&2
  exit 1
fi

export NEW_NAME NEW_EMAIL

git filter-repo --force --commit-callback '
import os, re

NEW_NAME  = os.environ["NEW_NAME"].encode()
NEW_EMAIL = os.environ["NEW_EMAIL"].encode()

# --- 1. 身份重写 ---------------------------------------------------------
def is_ai_identity(name, email):
    e = (email or b"").lower()
    n = (name or b"").lower()
    if e.endswith(b"noreply@anthropic.com"):
        return True
    if b"copilot-swe-agent" in n or b"copilot@users.noreply.github.com" in e:
        return True
    return False

if is_ai_identity(commit.author_name, commit.author_email):
    commit.author_name, commit.author_email = NEW_NAME, NEW_EMAIL
if is_ai_identity(commit.committer_name, commit.committer_email):
    commit.committer_name, commit.committer_email = NEW_NAME, NEW_EMAIL

# --- 2. 署名行剔除 -------------------------------------------------------
DROP = [
    # Co-Authored-By 指向 anthropic 的(型号名任意),大小写不敏感
    re.compile(rb"^\s*co-authored-by:.*noreply@anthropic\.com>?\s*$", re.I),
    # Copilot 的 co-author(本仓库当前没有,防未来再出现)
    re.compile(rb"^\s*co-authored-by:.*copilot.*@users\.noreply\.github\.com>?\s*$", re.I),
    # 会话回链
    re.compile(rb"^\s*claude-session:\s*https?://claude\.ai/.*$", re.I),
    re.compile(rb"^\s*https?://claude\.ai/code/session_\S*\s*$", re.I),
    # 生成页脚
    re.compile(rb"^\s*(\xf0\x9f\xa4\x96\s*)?generated with \[?claude code\]?.*$", re.I),
]

lines = commit.message.split(b"\n")
kept = [l for l in lines if not any(p.match(l) for p in DROP)]

# 剔完可能在结尾留下悬空空行,收尾整理
while kept and kept[-1].strip() == b"":
    kept.pop()
msg = b"\n".join(kept)
if msg:
    msg += b"\n"
commit.message = msg
'

echo
echo "重写完成。git-filter-repo 已移除 origin remote(防误推),需要时手动加回:"
echo "  git remote add origin <URL>"
