#!/bin/bash
# aarch64 交叉编译产物的运行时验证（qemu-user 模拟）
# 用法：wsl -e bash run_aarch64.sh [次数]
export PATH=/usr/local/bin:/usr/bin:/bin
export QEMU_LD_PREFIX=/opt/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu/aarch64-none-linux-gnu/libc

RUNS=${1:-5}
cd /mnt/d/git/netmech || exit 1

fail=0
for i in $(seq 1 "$RUNS"); do
    out=$(timeout 300 ./build-aarch64/bin/netmech_ut 2>&1)
    if echo "$out" | grep -q "结果: FAIL"; then
        fail=$((fail + 1))
        echo "--- run $i FAILED ---"
        echo "$out" | grep -A3 "FAIL\]" | head -6
    else
        echo "run $i: PASS"
    fi
done
echo "== netmech aarch64: 失败 $fail / $RUNS =="

cd /mnt/d/git/rtpmech || exit 1
rfail=0
for i in $(seq 1 "$RUNS"); do
    out=$(timeout 300 ./build-aarch64/bin/rtpmech_ut 2>&1)
    if echo "$out" | grep -q "结果: FAIL"; then
        rfail=$((rfail + 1))
        echo "--- rtpmech run $i FAILED ---"
        echo "$out" | grep -A3 "FAIL\]" | head -6
    else
        echo "rtpmech run $i: PASS"
    fi
done
echo "== rtpmech aarch64: 失败 $rfail / $RUNS =="
