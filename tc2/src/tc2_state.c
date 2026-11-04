/* tc2_state.c -- SAVE GAME / LOAD GAME for Time Crisis II (the Mac build; the Esc menu's slot lists, macos/eng_ui_simple.h). The same
 * design as Time Crisis 1's engine/ss22_state.c, for this board:
 *
 * A save is the whole machine: the R4650's registers and CP0 state, main RAM, the video RAM windows (not the backup RAM: settings and
 * rankings stay as they are now), the C412 / C417 / C421 memories, the C435 command processor and its render lists, the screen timing,
 * both H8s with their peripherals, the C352, and the link chip. It is taken at the start of a scheduler tick whose poll followed a LOOP'S
 * BACKWARD BRANCH: the lifted code polls on the taken path after the delay slot (rr_pc = the delay slot), so the instruction before rr_pc
 * is the branch and its target is where the program goes next -- a place it can be resumed with no C frames under it. Not inside an
 * exception. A load checks the whole file first, applies every pass and jumps back to the top of the run (src/tc2_main.c), which resumes
 * the program at the saved target.
 *
 * Files: saves/tc2_slot<N>.sav beside the game's settings: a header (magic, version, sizes, resume PC, time, a description) and the
 * state, zlib-compressed (main RAM is 16 MB, mostly zeros). A save whose state layout differs from this build's is refused. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <zlib.h>
#include "lift_rt.h"
#include "../../engine/eng_state.h"

void tc2_cpu_state(eng_st *s); void tc2_mem_state(eng_st *s); void tc2_chips_state(eng_st *s); void tc2_c435_state(eng_st *s);
void tc2_screen_state(eng_st *s); void tc2_ctl_state(eng_st *s); void tc2_link_state(eng_st *s); void tc2_h8_state(eng_st *s);
void eng_ui_set_hint(const char *text, int frames);
void eng_state_request(int save, int slot);
uint32_t rr_read(uint32_t a, int size);
extern uint8_t tc2_ram[0x1000000];
extern uint32_t rr_pc, rr_frame;
extern int rr_in_irq;

jmp_buf  g_tc2_resume_jb;
uint32_t g_tc2_resume_pc;
int      g_tc2_state_req;                    /* 1 save, 2 load */
int      g_tc2_state_test = -1;              /* ENG_STATE_TEST (-1 not read yet) */
static int req_slot, save_ticks;

#define STATE_VERSION 1
typedef struct {
    char     magic[8];                       /* "RLTC2ST\0" */
    uint32_t version, body, packed;
    uint32_t pc;
    int64_t  when;
    char     desc[64];
} state_hdr;

static void passes(eng_st *s)
{
    tc2_cpu_state(s); tc2_mem_state(s); tc2_chips_state(s); tc2_c435_state(s); tc2_screen_state(s); tc2_ctl_state(s); tc2_link_state(s);
    tc2_h8_state(s);
}
static size_t body_size(void) { eng_st s = { 0 }; passes(&s); return s.pos; }
static void slot_path(int slot, char *p, size_t n) { snprintf(p, n, "saves/tc2_slot%d.sav", slot + 1); }

/* the target of the loop branch just before rr_pc (its delay slot): a backward beq / bne / blez / bgtz (and the "likely" forms), a
 * bltz / bgez (not the linking ones), a bc1f / bc1t, or a j; 0 = not one */
static int loop_target(uint32_t *t)
{
    const uint32_t bpc = rr_pc - 4, w = rr_read(bpc, 4), op = w >> 26;
    const int32_t off = (int32_t)(int16_t)(w & 0xFFFF) * 4;
    if ((op >= 4 && op <= 7) || (op >= 0x14 && op <= 0x17)) *t = bpc + 4 + (uint32_t)off;
    else if (op == 1 && ((w >> 16) & 0x1F) <= 3) *t = bpc + 4 + (uint32_t)off;
    else if (op == 0x11 && ((w >> 21) & 0x1F) == 8) *t = bpc + 4 + (uint32_t)off;
    else if (op == 2) *t = ((bpc + 4) & 0xF0000000u) | ((w & 0x3FFFFFFu) << 2);
    else return 0;
    return *t <= bpc;
}

static void do_save(uint32_t resume)
{
    const size_t n = body_size();
    uint8_t *raw = malloc(n);
    uLongf packed = compressBound((uLong)n);
    uint8_t *z = malloc(sizeof(state_hdr) + packed);
    if (!raw || !z) { free(raw); free(z); eng_ui_set_hint("Could not save: out of memory", 240); return; }
    eng_st s = { 1, raw, 0, n, 0 };
    passes(&s);
    const int zok = !s.bad && compress2(z + sizeof(state_hdr), &packed, raw, (uLong)n, 1) == Z_OK;
    free(raw);
    state_hdr h; memset(&h, 0, sizeof h);
    memcpy(h.magic, "RLTC2ST", 8); h.version = STATE_VERSION; h.body = (uint32_t)n; h.packed = (uint32_t)packed; h.pc = resume; h.when = (int64_t)time(NULL);
    snprintf(h.desc, sizeof h.desc, "%s", tc2_ram[0x2CEE47] ? "in a game" : "title / demo");
    memcpy(z, &h, sizeof h);
    char p[256], tmp[280]; slot_path(req_slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    mkdir("saves", 0755);
    FILE *f = zok ? fopen(tmp, "wb") : NULL;
    const int ok = f && fwrite(z, 1, sizeof h + packed, f) == sizeof h + packed;
    if (f) fclose(f);
    free(z);
    if (ok && rename(tmp, p) == 0) {
        char m[64]; snprintf(m, sizeof m, "Saved to slot %d", req_slot + 1); eng_ui_set_hint(m, 180);
        fprintf(stderr, "[STATE] saved slot %d: frame %u, resume %08X, %zu bytes (%lu packed)\n", req_slot + 1, rr_frame, resume, n, (unsigned long)packed);
    } else { remove(tmp); eng_ui_set_hint("Could not save the game (disk full?)", 240); fprintf(stderr, "[STATE] save to %s failed\n", p); }
}

static void do_load(void)
{
    char p[256]; slot_path(req_slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) { eng_ui_set_hint("That slot is empty", 180); return; }
    fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
    const size_t n = body_size();
    state_hdr h; uint8_t *z = NULL, *raw = NULL;
    const int head = len >= (long)sizeof h && fread(&h, 1, sizeof h, f) == sizeof h;
    const int fits = head && !memcmp(h.magic, "RLTC2ST", 8) && h.version == STATE_VERSION && h.body == n && (size_t)len == sizeof h + h.packed;
    int ok = 0;
    if (fits && (z = malloc(h.packed)) && (raw = malloc(n)) && fread(z, 1, h.packed, f) == h.packed) {
        uLongf got = (uLongf)n;
        ok = uncompress(raw, &got, z, h.packed) == Z_OK && got == n;
    }
    fclose(f); free(z);
    if (!ok) {
        free(raw);
        eng_ui_set_hint(fits ? "Could not read that save" : "That save is from another version of the game", 240);
        fprintf(stderr, "[STATE] %s: not loaded (%s)\n", p, fits ? "read error" : "another format");
        return;
    }
    eng_st s = { 2, raw, 0, n, 0 };
    passes(&s);
    free(raw);
    char m[64]; snprintf(m, sizeof m, "Loaded slot %d", req_slot + 1); eng_ui_set_hint(m, 180);
    fprintf(stderr, "[STATE] loaded slot %d: frame %u, resume %08X\n", req_slot + 1, rr_frame, h.pc);
    g_tc2_resume_pc = h.pc;
    g_tc2_state_req = 0;
    longjmp(g_tc2_resume_jb, 1);
}

/* ENG_STATE_TEST=S:L (tests): a save into slot 8 at frame S, its load at frame L; every 50 frames from S to S+400 a fingerprint of the
 * registers and main RAM -- the same frames before and after the load must print the same */
static void state_test(void)
{
    static long ts = -1, tl = -1; static uint32_t last = ~0u; static int saved, loaded;
    if (g_tc2_state_test < 0) { const char *e = getenv("ENG_STATE_TEST"); g_tc2_state_test = e && sscanf(e, "%ld:%ld", &ts, &tl) == 2; if (!g_tc2_state_test) return; }
    if (rr_frame == last) return;
    last = rr_frame;
    if ((long)rr_frame == ts && !saved) { saved = 1; eng_state_request(1, 7); }
    if ((long)rr_frame == tl && !loaded) { loaded = 1; eng_state_request(0, 7); }
    if ((long)rr_frame > ts && (rr_frame - (uint32_t)ts) % 50 == 0 && (long)rr_frame <= ts + 400) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof R; i++) { h ^= R[i]; h *= 1099511628211ull; }
        for (size_t i = 0; i < sizeof tc2_ram; i++) { h ^= tc2_ram[i]; h *= 1099511628211ull; }
        fprintf(stderr, "[STATE-TEST] frame %u fingerprint %016llx%s\n", rr_frame, (unsigned long long)h, loaded ? " (after the load)" : "");
    }
}

/* src/tc2_cpu.c rr_tick, while a request is pending (or the test runs) */
void tc2_state_poll(void)
{
    if (g_tc2_state_test) state_test();
    if (!g_tc2_state_req) return;
    if (g_tc2_state_req == 2) { g_tc2_state_req = 0; do_load(); return; }
    uint32_t t;
    if (!rr_in_irq && loop_target(&t)) { g_tc2_state_req = 0; do_save(t); return; }
    if (++save_ticks > 20000) { g_tc2_state_req = 0; eng_ui_set_hint("Could not save just now: try again", 240); fprintf(stderr, "[STATE] no loop branch to save at\n"); }
}

/* the menu (macos/eng_ui_simple.h) */
int  eng_state_slots(void) { return 8; }
void eng_state_request(int save, int slot) { req_slot = slot; save_ticks = 0; g_tc2_state_req = save ? 1 : 2; }
int  eng_state_slot_info(int slot, char *buf, size_t n)
{
    char p[256]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    state_hdr h;
    if (!f || fread(&h, 1, sizeof h, f) != sizeof h || memcmp(h.magic, "RLTC2ST", 8)) { if (f) fclose(f); snprintf(buf, n, "empty"); return 0; }
    fclose(f);
    const time_t w = (time_t)h.when; struct tm tm; localtime_r(&w, &tm);
    char d[32]; strftime(d, sizeof d, "%d %b %H:%M", &tm);
    h.desc[sizeof h.desc - 1] = 0;
    snprintf(buf, n, "%s%s%s", d, h.desc[0] ? "  -  " : "", h.desc);
    return 1;
}
