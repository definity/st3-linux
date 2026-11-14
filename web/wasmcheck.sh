#!/bin/bash
# wasmcheck.sh — prove the WebAssembly build of the emulator core is exact: run the headless test driver natively and
# as WebAssembly (under node) with the same songs and keys, and require byte-identical audio, screen and log.
# usage: web/wasmcheck.sh [SONG...]      (default: a few bundled songs: sample, MOD, FM)   needs emcc on PATH
set -u
W=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/wasmcheck-XXXX")
NODE=$(command -v node || true)
[ -n "${EMSDK_NODE:-}" ] && NODE=$EMSDK_NODE
[ -z "$NODE" ] && { echo "wasmcheck: node not found"; exit 2; }
make -s -C "$W/st3emu" st3head || exit 2
SRC="cpu.c machine.c io.c vga.c bios.c dos.c keys.c opl.c mixer.c vgafb.c ems.c"
emcc -O3 -w -sNODERAWFS=1 -sENVIRONMENT=node -sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1 -o "$OUT/st3head.js" \
    $(for f in $SRC; do echo "$W/st3emu/src/$f"; done) "$W/st3emu/tests/headless.c" -lm || exit 2

KEYS="{W2500}{SPACE}{W500}{CTRL-l}{W1500}{DOWN}{W500}{ENTER}{W800}y{W9000}{F5}{W1500}{PGDN}{W10000}"
[ $# -eq 0 ] && set -- "$W/dist/s/ARMANI.S3M" "$W/dist/s/TECHNOLO.MOD" "$W/dist/s/FMTEST.S3M"
fail=0
for src in "$@"; do
    song=$(basename "$src")
    for v in native wasm; do
        d="$OUT/$v-$song"; mkdir -p "$d"; cp "$W/dist/ST3.SET" "$src" "$d/"
        if [ $v = native ]; then (cd "$d" && "$W/st3emu/st3head" . "$W/ST3U.EXE" 28 "$KEYS" out) >/dev/null 2>&1
        else (cd "$d" && "$NODE" "$OUT/st3head.js" . "$W/ST3U.EXE" 28 "$KEYS" out) >/dev/null 2>&1; fi
    done
    res=""
    for f in wav ppm log; do
        if cmp -s "$OUT/native-$song/out.$f" "$OUT/wasm-$song/out.$f" && [ -s "$OUT/native-$song/out.$f" ]; then res="$res $f:same"
        else res="$res $f:DIFFERENT"; fail=1; fi
    done
    echo "$song:$res"
done
[ $fail = 0 ] && echo "wasmcheck: all identical" || echo "wasmcheck: MISMATCH (outputs in $OUT)"
exit $fail
