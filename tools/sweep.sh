#!/bin/bash
# sweep.sh — regression sweep: load and play every song in a directory headlessly in ST3U.EXE.
# usage: tools/sweep.sh SONGDIR OUTDIR [JOBS]
# Each song runs in its own scratch dir (song + ST3.SET), is loaded via Ctrl-L, played with F5 for ~20 s,
# and reported as: song, loaded?, CPU errors, unhandled INTs, other emulator complaints, EMS handles, RMS of last 10 s.
# Results: OUTDIR/results.tsv (plus per-song .log/.wav/.ppm).
set -u
W=$(cd "$(dirname "$0")/.." && pwd)
SONGS=$(cd "$1" && pwd); OUT=$(mkdir -p "$2" && cd "$2" && pwd); JOBS=${3:-$(nproc)}
KEYS="{W2500}{SPACE}{W500}{CTRL-l}{W1500}{DOWN}{W500}{ENTER}{W800}y{W12000}{F3}{W800}{F5}{W20000}"
export W OUT KEYS

one() {
    song=$1; base=$(basename "$song"); tag=${base%.*}; d=$OUT/run/$tag
    mkdir -p "$d"; cp "$W/sandbox/ST3.SET" "$song" "$d/"
    (cd "$d" && timeout 300 "$W/st3emu/st3head" . "$W/ST3U.EXE" 40 "$KEYS" "$OUT/$tag" >"$OUT/$tag.stderr" 2>&1)
    rc=$?
    log=$OUT/$tag.log
    loaded=$(grep -ci "OPEN '$base'" "$log")
    cpu=$(grep -c "CPU error" "$log"); unh=$(grep -c "UNHANDLED INT" "$log")
    other=$(grep -c "unhandled DSP\|unsupported INT 67" "$log"); ems=$(grep -c "EMS 43" "$log")
    rms=$(python3 -I -c "
import array,math,sys
try: d=open(sys.argv[1],'rb').read()[44:]
except OSError: print(-1); sys.exit()
a=array.array('h'); a.frombytes(d[:len(d)//2*2]); x=a[-48000*2*10:]
print(int(math.sqrt(sum(v*v for v in x)/max(1,len(x)))))" "$OUT/$tag.wav")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$base" "$rc" "$loaded" "$cpu" "$unh" "$other" "$ems" "$rms"
    rm -rf "$d"
}
export -f one
printf 'song\trc\tloaded\tcpu_err\tunh_int\tother\tems_handles\trms_last10s\n' >"$OUT/results.tsv"
find "$SONGS" -maxdepth 1 -type f \( -iname '*.s3m' -o -iname '*.mod' \) -print0 | sort -z |
    xargs -0 -n1 -P "$JOBS" bash -c 'one "$0"' >>"$OUT/results.tsv"
echo "done: $(($(wc -l <"$OUT/results.tsv") - 1)) songs -> $OUT/results.tsv"
