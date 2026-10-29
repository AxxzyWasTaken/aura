#!/bin/sh
# Host check of the compressed built-in boot logos (Makefile logo rules, fb.c builtin_logo()).
#
#   sh tests/run-logo.sh [name [directory]]      (default: aurora data)
#
# Compresses <directory>/<name>_{48,128,256}.bin as the Makefile does and checks that aura's tinf
# inflates each to exactly the original pixels, and refuses a buffer one byte too small.
set -eu
cd "$(dirname "$0")/.."
name=${1:-aurora}
dir=${2:-data}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer \
    -fno-sanitize-recover=undefined -Isrc tests/logo_host.c src/tinf/tinflate.c \
    src/tinf/tinfgzip.c src/tinf/crc32.c -o "$tmp/logo-host"
for n in 48 128 256; do
    gzip -9 -n -c "$dir/${name}_$n.bin" > "$tmp/${name}_$n.bin.gz"
    "$tmp/logo-host" "$n" "$dir/${name}_$n.bin" "$tmp/${name}_$n.bin.gz"
done
