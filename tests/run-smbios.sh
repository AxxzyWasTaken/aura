#!/bin/sh
# Host test of the SMBIOS node aura hands U-Boot, on Macs' ADTs and device trees.
#
#   sh tests/run-smbios.sh <mac.adt> <mac.dtb> [<mac.adt> <mac.dtb> ...]
#
# The ADT is a binary dump of a Mac's Apple device tree and the DTB that Mac's device tree (a
# kernel build's, or /sys/firmware/fdt). Both are read here and never copied into the tree.
# tests/smbios_host.c runs kboot.c's dt_set_smbios() and dt_set_smbios_firmware() with their helpers, copied out of
# src/kboot.c below, with aura's ADT reader (rust/src/adt.rs, built for the host) and libfdt,
# twice (the node must come out the same), once as an Armv8 and once as an Armv9 (SME) CPU.
set -eu
cd "$(dirname "$0")/.."
root=$(pwd)
[ $# -ge 2 ] && [ $(($# % 2)) = 0 ] || {
    echo "usage: run-smbios.sh <mac.adt> <mac.dtb> [<mac.adt> <mac.dtb> ...]" >&2
    exit 2
}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# aura's ADT reader, built for the host as tests/run-t6031-handoff.sh does
mkdir -p "$tmp/rs" "$tmp/src"
if grep -q 'CStr::from_bytes_until_nul(&self.name)' rust/src/adt.rs; then
    sed 's|CStr::from_bytes_until_nul(&self.name)|CStr::from_bytes_until_nul(unsafe { \&*(\&self.name as *const [c_char; 32] as *const [u8; 32]) })|' \
        rust/src/adt.rs > "$tmp/rs/adt.rs"
elif grep -q 'from_raw_parts(self.name.as_ptr() as \*const u8, 32)' rust/src/adt.rs; then
    cp rust/src/adt.rs "$tmp/rs/adt.rs"
else
    echo "FAIL: rust/src/adt.rs changed; update the host build in $0"
    exit 1
fi
printf '#![allow(non_camel_case_types, dead_code, unused_imports)]\ntype c_size_t = usize;\nmod adt;\n' \
    > "$tmp/rs/lib.rs"
RUSTUP_TOOLCHAIN=${RUSTUP_TOOLCHAIN:-1.89.0} rustc --edition 2021 --crate-type staticlib -O \
    -o "$tmp/rs/libadt_host.a" "$tmp/rs/lib.rs"

# kboot.c's SMBIOS code, copied out as it is
{
    printf '#include <string.h>\n#include "adt.h"\n#include "types.h"\n#include "utils.h"\n'
    printf '#include "libfdt/libfdt.h"\n'
    printf '#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)\n'
    printf 'static void *dt;\n'
    # what the copied code takes from firmware.c, main.c and rust/src/usb4.rs
    printf 'struct fw_version_info { const char *string; const char *iboot; };\n'
    printf 'struct fw_version_info system_firmware = { "27.0", "mBoot-20457.1.29" };\n'
    printf 'struct fw_version_info os_firmware = { "26.6.2", "mBoot-18000.161.10" };\n'
    printf 'const char *const aura_version = "1.0.0b1-host";\n'
    printf 'int rust_platform_uuid(u8 *out) { for (int i = 0; i < 16; i++) out[i] = 0x11 * i; return 0; }\n'
    awk '/^ \* SMBIOS values for U-Boot/ { on = 1; print "/*" } on { print } on && /^static int dt_set_smbios_firmware[(]/ { body = 1 } body && /^}/ { exit }' \
        src/kboot.c
    printf 'int host_set_smbios(void *d)\n{\n    dt = d;\n    return dt_set_smbios() || dt_set_smbios_firmware();\n}\n'
} > "$tmp/src/kboot_smbios_copy.c"
for f in dt_cpu_summary dt_set_smbios dt_set_smbios_firmware dt_l2_summary dt_dram_data_rate; do
    grep -q "$f" "$tmp/src/kboot_smbios_copy.c" || {
        echo "FAIL: $f not found in src/kboot.c"
        exit 1
    }
done

cp tests/smbios_host/utils.h tests/smbios_host.c "$tmp/src/"
${CC:-cc} -std=gnu11 -D_GNU_SOURCE -O1 -g -Wall -Wno-unused-function \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined \
    -I"$tmp/src" -I"$root/src" -I"$root" \
    "$tmp"/src/*.c src/libfdt/fdt.c src/libfdt/fdt_ro.c src/libfdt/fdt_rw.c src/libfdt/fdt_wip.c \
    src/libfdt/fdt_sw.c src/libfdt/fdt_strerror.c "$tmp/rs/libadt_host.a" -o "$tmp/smbios-host"

export ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0}
fail=0
while [ $# -ge 2 ]; do
    for cpu in v8 sme; do
        echo "### $(basename "$1") $(basename "$2") $cpu"
        "$tmp/smbios-host" "$1" "$2" "$cpu" || fail=1
    done
    shift 2
done
[ $fail = 0 ] && echo "SMBIOS: every Mac passes"
exit $fail
