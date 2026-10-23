#!/bin/bash
# navimap 推送收尾：建远端仓库 + 推送
#
# 用法 A（推荐，有 token）：
#   scripts/push_navimap.sh <github_token>
#
# 用法 B（无 token）：先在浏览器建一个空的公开仓库
#   https://github.com/new   → 仓库名 navimap，owner a-long-long，Public
#   建好后直接执行本脚本（不带参数），它会跳过建仓、直接推送。
#
# 说明：
# - 推送本身走 SSH（~/.ssh/id_ed25519，GitHub 账号 a-long-long），已实证可用。
# - 建仓必须走 API（需要 token）或网页：GitHub 已关闭 "push 自动建仓"。
# - REPO_DIR 按脚本自身位置推导（曾经写死 /home/along/navimap，换机即废）。
set -u
REPO_DIR=$(cd "$(dirname "$0")/.." && pwd)
REMOTE=git@github.com:a-long-long/navimap.git
API=https://api.github.com
TMP_JSON=${TMPDIR:-/tmp}/_nr.json

cd "$REPO_DIR" || exit 1

echo "== 本地提交 =="
git log --oneline

# 1) 确保远端仓库存在
if [ $# -ge 1 ] && [ -n "${1:-}" ]; then
    TOKEN="$1"
    echo "== 用 token 建仓 =="
    code=$(curl -sS -o "$TMP_JSON" -w '%{http_code}' -X POST \
      -H "Authorization: token $TOKEN" -H "Accept: application/vnd.github+json" \
      "$API/user/repos" \
      -d '{"name":"navimap","description":"navimap: 纯 C 实现的离线地图渲染与路由（SDL2）","private":false,"auto_init":false,"has_issues":true,"has_wiki":false,"has_projects":false}')
    echo "建仓 HTTP=$code"
    NR_TMP_JSON="$TMP_JSON" python3 - <<'PY' 2>/dev/null
import json, os
try:
    d=json.load(open(os.environ['NR_TMP_JSON']))
    print("  ->", d.get('full_name') or d.get('message'))
except Exception:
    pass
PY
    if [ "$code" != "201" ] && [ "$code" != "422" ]; then
        echo "建仓失败（$code）。若提示已有同名仓库，说明已建好，可直接重跑不带 token 的版本。"
        exit 1
    fi
else
    echo "== 未提供 token：假定远端仓库已存在 =="
    code=$(curl -sS -o /dev/null -w '%{http_code}' "$API/repos/a-long-long/navimap")
    echo "远端探测 HTTP=$code"
    if [ "$code" != "200" ]; then
        echo
        echo "远端仓库不存在。请二选一："
        echo "  A) 在浏览器建空仓库：https://github.com/new"
        echo "     → owner: a-long-long   仓库名: navimap   可见性: Public"
        echo "     → 建好后重跑本脚本"
        echo "  B) 提供 token："
        echo "     → https://github.com/settings/tokens  生成（勾选 repo 权限）"
        echo "     → $0 <token>"
        exit 1
    fi
fi

# 2) 推送
git remote set-url origin "$REMOTE"
echo "== 推送 =="
GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=accept-new -o ConnectTimeout=30" \
    git push -u origin main

# 3) 校验
echo "== 校验 =="
git remote -v
git log --oneline -2
code=$(curl -sS -o /dev/null -w '%{http_code}' "$API/repos/a-long-long/navimap")
echo "远端 HTTP=$code  https://github.com/a-long-long/navimap"
