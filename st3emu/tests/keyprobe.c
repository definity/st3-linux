/* keyprobe.c — does ST3 react to a key?  Brings ST3 into a state with a key script, snapshots the whole machine,
 * then for each probe key: restore, press it, run 0.6 s (virtual), and compare all memory + CPU registers.
 * Two junk keys no keyboard can produce (scan 7Eh/7Fh, no ASCII) give the "ignored key" reference: bytes where the
 * junk runs differ from each other are places that merely record the last key code.  A probe key is IGNORED when it
 * differs from junk only at those places; otherwise ST3 HANDLES it (sanity probe: a key ST3 is known to use).
 * usage: keyprobe ROOT EXE "SETUP-KEYSCRIPT" PROBE-AX [SANITY-AX]    (AX as hex, e.g. 2106 for Ctrl-F)
 * prints: IGNORED or HANDLED (+ number of differing bytes) for the probe and the sanity key. */
#include "../src/machine.h"
#include "../src/keys.h"
#include <stdlib.h>
#include <string.h>

static uint8_t MEM[MEM_SIZE], SNAP[MEM_SIZE], R[4][MEM_SIZE];
static Machine M, MS, RM[4];

static void run_for(double secs) {
    double end = machine_vtime(&M) + secs;
    while (machine_vtime(&M) < end && !M.exited) machine_run(&M, 20000);
}
static void restore(void) { memcpy(MEM, SNAP, MEM_SIZE); M = MS; }
static void probe(int slot, uint16_t key) {
    restore(); machine_key(&M, key); run_for(0.6);
    memcpy(R[slot], MEM, MEM_SIZE); RM[slot] = M;
}
/* count bytes (memory + CPU regs + VGA state) differing between runs a and b, optionally skipping those in mask run pair */
static long diff(int a, int b, int ma, int mb) {
    long n = 0;
    for (long i = 0; i < MEM_SIZE; i++)
        if (R[a][i] != R[b][i] && !(ma >= 0 && R[ma][i] != R[mb][i])) n++;
    if (memcmp(RM[a].font, RM[b].font, sizeof RM[a].font)) n += 2000000;
    if (memcmp(RM[a].dac, RM[b].dac, sizeof RM[a].dac)) n += 4000000;
    return n;
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: %s ROOT EXE SETUP PROBE-AX [SANITY-AX]\n", argv[0]); return 1; }
    MachineConfig cfg = {0}; cfg.mips = 25.0; cfg.root = argv[1]; cfg.args = "";
    machine_init(&M, MEM, &cfg, getenv("KP_LOG") ? fopen(getenv("KP_LOG"), "w") : NULL);
    if (machine_load_exe(&M, argv[2])) { fprintf(stderr, "cannot load %s\n", argv[2]); return 1; }
    KeyScript ks; keyscript_parse(&ks, argv[3]);
    run_for(2.0);
    for (int i = 0; i < ks.n; i++) {
        if (ks.tok[i].wait_ms) run_for(ks.tok[i].wait_ms / 1000.0);
        else { machine_key(&M, ks.tok[i].key); run_for(0.3); }
    }
    run_for(1.0);
    fprintf(stderr, "setup: vtime %.2f exited %d icount %llu\n", machine_vtime(&M), M.exited, (unsigned long long)M.cpu.icount);
    memcpy(SNAP, MEM, MEM_SIZE); MS = M;
    probe(0, 0x7E00); probe(1, 0x0300);   /* ST3 key codes 1126 (0466h) and 1003 (03EBh): both bytes differ */
    probe(2, (uint16_t)strtol(argv[4], NULL, 16));
    long jj = diff(0, 1, -1, -1), pf = diff(2, 0, 0, 1);
    if (getenv("KP_SHOW")) { int k = 0;
        for (long i = 0; i < MEM_SIZE && k < 40; i++) if (R[2][i] != R[0][i] && R[0][i] == R[1][i]) { fprintf(stderr, " %05lX:%02X->%02X", i, R[0][i], R[2][i]); k++; }
        fprintf(stderr, "\n"); }
    printf("junk-vs-junk %ld bytes | probe %s: %s (%ld)", jj, argv[4], pf ? "HANDLED" : "IGNORED", pf);
    if (argc > 5) { probe(3, (uint16_t)strtol(argv[5], NULL, 16)); long ps = diff(3, 0, 0, 1);
                    printf(" | sanity %s: %s (%ld)", argv[5], ps ? "HANDLED" : "IGNORED", ps); }
    printf("\n");
    return 0;
}
