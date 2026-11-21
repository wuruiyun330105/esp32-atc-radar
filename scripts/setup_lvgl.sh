#!/usr/bin/env bash
# 下载 LVGL 9.3.0 到 third_party/lvgl（PC 模拟器用，设备端不需要）
# 用法：bash scripts/setup_lvgl.sh
set -euo pipefail

VER="9.3.0"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="$ROOT/third_party/lvgl"

if [ -f "$DEST/src/lvgl.h" ]; then
    echo "LVGL 已存在：$DEST"
    exit 0
fi

for cmd in curl tar; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "缺少 $cmd，请先安装"; exit 1; }
done

mkdir -p "$ROOT/third_party"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# GitHub 直连优先，失败自动换镜像（国内网络经常连不上 github.com）
URLS=(
    "https://github.com/lvgl/lvgl/archive/refs/tags/v${VER}.tar.gz"
    "https://gh-proxy.com/https://github.com/lvgl/lvgl/archive/refs/tags/v${VER}.tar.gz"
    "https://ghfast.top/https://github.com/lvgl/lvgl/archive/refs/tags/v${VER}.tar.gz"
)

ok=0
for u in "${URLS[@]}"; do
    echo "下载：$u"
    # 连不上就尽快换源：国内直连 github 通常 8 秒内就 timeout
    if curl -fL --progress-bar --connect-timeout 8 --retry 1 --retry-delay 1 \
            -o "$TMP/lvgl.tar.gz" "$u"; then
        ok=1
        break
    fi
    echo "  失败，换下一个源"
done

if [ "$ok" -ne 1 ]; then
    echo ""
    echo "三个源都下不动。手动下载后放到 third_party/lvgl 即可："
    echo "  https://github.com/lvgl/lvgl/archive/refs/tags/v${VER}.tar.gz"
    echo "  解压后把 lvgl-${VER} 改名成 lvgl，放进 $ROOT/third_party/"
    exit 1
fi

echo "解压..."
tar -xzf "$TMP/lvgl.tar.gz" -C "$TMP"

SRC="$TMP/lvgl-${VER}"
[ -d "$SRC" ] || SRC="$(find "$TMP" -maxdepth 1 -type d -name 'lvgl*' ! -path "$TMP" | head -1)"
[ -d "$SRC/src" ] || { echo "解压结果异常：$SRC"; exit 1; }

rm -rf "$DEST"
mv "$SRC" "$DEST"

# 记录版本，方便以后核对
printf '%s\n' "$VER" > "$DEST/.lvgl_version"

echo ""
echo "完成：$DEST"
echo "下一步："
echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build build -j"
echo "  ./build/sim"
