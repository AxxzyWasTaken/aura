#!/bin/sh
# Host test of the G15 GPU setup (src/kboot_gpu.c) on T6030 and T8122.
#
#   sh tests/run-kboot-gpu-g15.sh [BASE_REF]
#
# Runs every scenario of tests/kboot_gpu_g15_host.c against this tree. With BASE_REF (a git ref
# of this repository, e.g. aurora7), the T6030 scenarios are also built against that ref's
# kboot_gpu.c and firmware.c, and their DT, bus/PMGR traces and logs must be identical. If that
# ref has the T8122 setup (e.g. aurora12), so are the T8122 scenarios without the stand-in switch.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
base=${1:-}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

build() { # build <tree> <out> [extra cflags]
    tree=$1 out=$2
    shift 2
    mkdir -p "$out/src"
    cp "$tree/src/kboot_gpu.c" "$tree/src/firmware.c" "$out/src/"
    cp "$root"/tests/g15_host/*.h "$root/tests/kboot_gpu_g15_host.c" "$out/src/"
    extra=
    [ -f "$tree/src/kboot_gpu_t8122_power.c" ] && extra="$tree/src/kboot_gpu_t8122_power.c"
    ${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wno-unused-function \
        -Wno-format-truncation -fsanitize=address,undefined -fno-omit-frame-pointer \
        -fno-sanitize-recover=undefined "$@" \
        -I"$out/src" -I"$tree/src" -I"$tree" \
        "$out/src/kboot_gpu_g15_host.c" "$out/src/firmware.c" $extra \
        "$tree"/src/libfdt/fdt.c "$tree"/src/libfdt/fdt_ro.c "$tree"/src/libfdt/fdt_rw.c \
        "$tree"/src/libfdt/fdt_wip.c "$tree"/src/libfdt/fdt_sw.c \
        "$tree"/src/libfdt/fdt_empty_tree.c "$tree"/src/libfdt/fdt_addresses.c \
        -lm -o "$out/kboot-gpu-g15"
}

build "$root" "$tmp/new" -DG15_NEW
export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}

pass=0
for s in $("$tmp/new/kboot-gpu-g15" list); do
    "$tmp/new/kboot-gpu-g15" "$s" > "$tmp/new/$s.out" 2>&1 || {
        cat "$tmp/new/$s.out"
        echo "FAIL: $s"
        exit 1
    }
    pass=$((pass + 1))
done
echo "PASS: $pass G15 scenarios"

if [ -n "$base" ]; then
    mkdir -p "$tmp/base-tree"
    git archive "$base" | tar -x -C "$tmp/base-tree"
    what=T6030
    if grep -q g15_soc_t8122 "$tmp/base-tree/src/kboot_gpu.c"; then
        build "$tmp/base-tree" "$tmp/base" -DG15_T8122_BASE
        what="T6030 and T8122 (no stand-in switch)"
    else
        build "$tmp/base-tree" "$tmp/base"
    fi
    same=0
    for s in $("$tmp/base/kboot-gpu-g15" list); do
        "$tmp/base/kboot-gpu-g15" "$s" > "$tmp/base/$s.out" 2>&1 || {
            cat "$tmp/base/$s.out"
            echo "FAIL: $s on $base"
            exit 1
        }
        if ! cmp -s "$tmp/base/$s.out" "$tmp/new/$s.out"; then
            diff -u "$tmp/base/$s.out" "$tmp/new/$s.out" | head -60
            echo "FAIL: $s differs from $base"
            exit 1
        fi
        same=$((same + 1))
    done
    echo "PASS: $same $what scenarios identical to $base (DT, bus and PMGR traces, log)"
fi
