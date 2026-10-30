#!/usr/bin/env bash
# =============================================================================
#  Linux / WSL 一键构建 + 测试
# =============================================================================
#  用法:  ./build_linux.sh
#         BUILD_DIR=out ./build_linux.sh      # 指定构建目录
#         JOBS=4 ./build_linux.sh             # 指定并行度
#
#  依赖: g++ >= 9（需要 C++17）、cmake >= 3.15
#        Ubuntu/Debian:  sudo apt install -y build-essential cmake
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR=${BUILD_DIR:-build-linux}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 4)}

echo "== 配置 =="
cmake -S . -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Release \
      -DNETMECH_BUILD_TESTS=ON \
      -DNETMECH_BUILD_EXAMPLES=ON

echo "== 编译（-j$JOBS）=="
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "== 单元测试 =="
ctest --test-dir "$BUILD_DIR" --output-on-failure --timeout 300

echo
echo "== 完成 =="
echo "测试程序: $BUILD_DIR/netmech_ut"
echo "示例    : $BUILD_DIR/netmech_echo / $BUILD_DIR/netmech_udp"
