/* ss22_state.c -- SAVE GAME / LOAD GAME for the lifted Super 22 games (the Mac build; the Esc menu's slot lists, macos/eng_ui_simple.h).
 *
 * A save is the whole machine at one moment: the 68K's registers, every RAM of the board, the master DSP, the sound MCU with its timers
 * and the C352, the link chip, and the scheduler's place in the frame (engine/eng_state.h lists the passes). It is taken at the start of
 * a scheduler tick whose poll followed a LOOP'S BACKWARD BRANCH (Bcc / DBcc, decoded from the program at rr_pc_now): the lifted code
 * polls only on the taken path, so the next instruction is the branch target -- a place the program can be resumed with no C frames
 * under it (engine/lift_cpu.c rr_state). Not inside an interrupt. Usually the first tick qualifies; the menu closes and the save follows
 * within a frame or two.
 *
 * A load reads and checks the whole file first (magic, game, version, exact size), then applies every pass and jumps back to the top of
 * the run (engine/ss22_run.c g_ss22_resume_jb), which resumes the program at the saved target.
 *
 * Files: saves/slot<N>.sav beside the game's settings. Header: magic, game tag, format version, body size, resume PC, time, a description
 * (the game's state_desc). A save from a build whose state layout differs (another size) is refused, never half-applied. The EEPROM (the
 * player's settings and records) is not in a save. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "ss22_game.h"
#include "lift_rt.h"
#include "lift_cpu.h"
#include "eng_state.h"

jmp_buf  g_ss22_resume_jb;
uint32_t g_ss22_resume_pc;
int      g_ss22_state_req;                   /* 1 save, 2 load (the slot in req_slot) */
static int req_slot, save_ticks;

#define STATE_VERSION 1
typedef struct {
    char     magic[8];                       /* "RLS22ST\0" */
    char     tag[8];
    uint32_t version, body;
    uint32_t pc;
    int64_t  when;
    char     desc[64];
} state_hdr;

void eng_ui_set_hint(const char *text, int frames);
void eng_state_request(int save, int slot);
static void passes(eng_st *s)
{
    rr_state(s); ss22_board_state(s); ss22_dsp_state(s); ss22_snd_state(s); ss22_run_state(s); frame_rule_state(s); c139_state(s);
}
static size_t body_size(void) { eng_st s = { 0 }; passes(&s); return s.pos; }
static void slot_path(int slot, char *p, size_t n) { snprintf(p, n, "saves/%s_slot%d.sav", g_ss22_game->lname, slot + 1); }

/* the target of the loop branch at pc (Bcc/BRA with a backward 8/16/32-bit displacement, or DBcc); 0 = not one */
static int loop_target(uint32_t pc, uint32_t *t)
{
    const uint32_t op = rr_read(pc, 2);
    int32_t d;
    if ((op & 0xF0F8) == 0x50C8) d = (int16_t)rr_read(pc + 2, 2);                       /* DBcc Dn,<disp16> */
    else if ((op & 0xF000) == 0x6000 && (op & 0x0F00) != 0x0100) {                     /* Bcc / BRA (not BSR) */
        d = (int8_t)(op & 0xFF);
        if ((op & 0xFF) == 0) d = (int16_t)rr_read(pc + 2, 2);
        else if ((op & 0xFF) == 0xFF) d = (int32_t)rr_read(pc + 2, 4);
    } else return 0;
    *t = pc + 2 + (uint32_t)d;
    return d < 0;
}

static void do_save(uint32_t resume)
{
    const size_t n = body_size();
    uint8_t *b = malloc(sizeof(state_hdr) + n);
    if (!b) { eng_ui_set_hint("Could not save: out of memory", 240); return; }
    state_hdr h; memset(&h, 0, sizeof h);
    memcpy(h.magic, "RLS22ST", 8); snprintf(h.tag, sizeof h.tag, "%s", g_ss22_game->tag);
    h.version = STATE_VERSION; h.body = (uint32_t)n; h.pc = resume; h.when = (int64_t)time(NULL);
    if (g_ss22_game->state_desc) g_ss22_game->state_desc(h.desc, sizeof h.desc);
    memcpy(b, &h, sizeof h);
    eng_st s = { 1, b + sizeof h, 0, n, 0 };
    passes(&s);
    char p[256], tmp[280]; slot_path(req_slot, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    mkdir("saves", 0755);
    FILE *f = fopen(tmp, "wb");
    const bool ok = f && !s.bad && fwrite(b, 1, sizeof h + n, f) == sizeof h + n;
    if (f) fclose(f);
    free(b);
    if (ok && rename(tmp, p) == 0) {
        char m[64]; snprintf(m, sizeof m, "Saved to slot %d", req_slot + 1); eng_ui_set_hint(m, 180);
        fprintf(stderr, "[STATE] saved slot %d: frame %u, resume %06X, %zu bytes\n", req_slot + 1, rr_frame, resume, n);
    } else { remove(tmp); eng_ui_set_hint("Could not save the game (disk full?)", 240); fprintf(stderr, "[STATE] save to %s failed\n", p); }
}

static void do_load(void)
{
    char p[256]; slot_path(req_slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) { eng_ui_set_hint("That slot is empty", 180); return; }
    fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
    const size_t n = body_size();
    state_hdr h;
    uint8_t *b = NULL;
    const bool head = len >= (long)sizeof h && fread(&h, 1, sizeof h, f) == sizeof h;
    const bool fits = head && !memcmp(h.magic, "RLS22ST", 8) && !strncmp(h.tag, g_ss22_game->tag, sizeof h.tag) && h.version == STATE_VERSION &&
                      h.body == n && (size_t)len == sizeof h + n;
    if (fits && (b = malloc(n)) && fread(b, 1, n, f) == n) {
        fclose(f);
        eng_st s = { 2, b, 0, n, 0 };
        passes(&s);                                           /* sizes were checked: the pass cannot run out */
        free(b);
        char m[64]; snprintf(m, sizeof m, "Loaded slot %d", req_slot + 1); eng_ui_set_hint(m, 180);
        fprintf(stderr, "[STATE] loaded slot %d: frame %u, resume %06X\n", req_slot + 1, rr_frame, h.pc);
        g_ss22_resume_pc = h.pc;
        g_ss22_state_req = 0;
        longjmp(g_ss22_resume_jb, 1);
    }
    fclose(f); free(b);
    eng_ui_set_hint(fits ? "Could not read that save" : "That save is from another version of the game", 240);
    fprintf(stderr, "[STATE] %s: not loaded (%s)\n", p, fits ? "read error" : "another format");
}

/* ENG_STATE_TEST=S:L (tests): request a save into slot 8 at frame S and its load at frame L; every 50 frames from S to S+400 a
 * fingerprint of the 68K registers, work RAM and polygon RAM -- the same frames before and after the load must print the same */
int g_ss22_state_test = -1;
static void state_test(void)
{
    static long ts = -1, tl = -1; static uint32_t last = ~0u; static int saved, loaded;
    if (g_ss22_state_test < 0) { const char *e = getenv("ENG_STATE_TEST"); g_ss22_state_test = e && sscanf(e, "%ld:%ld", &ts, &tl) == 2; if (!g_ss22_state_test) return; }
    if (rr_frame == last) return;
    last = rr_frame;
    if ((long)rr_frame == ts && !saved) { saved = 1; eng_state_request(1, 7); }
    if ((long)rr_frame == tl && !loaded) { loaded = 1; eng_state_request(0, 7); }
    if ((long)rr_frame > ts && (rr_frame - (uint32_t)ts) % 50 == 0 && (long)rr_frame <= ts + 400) {
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof R; i++) { h ^= R[i]; h *= 1099511628211ull; }
        for (size_t i = 0; i < sizeof g_ss22.wram; i++) { h ^= g_ss22.wram[i]; h *= 1099511628211ull; }
        for (size_t i = 0; i < SS22_POLY_WORDS; i++) { h ^= g_ss22.poly[i]; h *= 1099511628211ull; }
        fprintf(stderr, "[STATE-TEST] frame %u fingerprint %016llx%s\n", rr_frame, (unsigned long long)h, loaded ? " (after the load)" : "");
        if (rr_frame - (uint32_t)ts == 200 && g_ss22_game) {         /* the picture too (a window run): the same frame, before and after */
            extern void ss22_host_shot(const char *path); mkdir("screenshots", 0755);
            char p[96]; snprintf(p, sizeof p, "screenshots/state_%u_%s.ppm", rr_frame, loaded ? "after" : "before"); ss22_host_shot(p);
        }
    }
}

/* engine/ss22_run.c rr_tick, while a request is pending */
void ss22_state_poll(void)
{
    if (g_ss22_state_test) state_test();
    if (!g_ss22_state_req) return;
    if (g_ss22_state_req == 2) { g_ss22_state_req = 0; do_load(); return; }
    uint32_t t;
    if (!rr_in_irq && loop_target(rr_pc_now, &t)) { g_ss22_state_req = 0; do_save(t); return; }
    if (++save_ticks > 4000) { g_ss22_state_req = 0; eng_ui_set_hint("Could not save just now: try again", 240); fprintf(stderr, "[STATE] no loop branch to save at\n"); }
}

/* the menu (macos/eng_ui_simple.h) */
int  eng_state_slots(void) { return 8; }
void eng_state_request(int save, int slot) { req_slot = slot; save_ticks = 0; g_ss22_state_req = save ? 1 : 2; }
int  eng_state_slot_info(int slot, char *buf, size_t n)       /* 1 = a save is there (buf: when and where), 0 = empty */
{
    char p[256]; slot_path(slot, p, sizeof p);
    FILE *f = fopen(p, "rb");
    state_hdr h;
    if (!f || fread(&h, 1, sizeof h, f) != sizeof h || memcmp(h.magic, "RLS22ST", 8)) { if (f) fclose(f); snprintf(buf, n, "empty"); return 0; }
    fclose(f);
    const time_t w = (time_t)h.when; struct tm tm; localtime_r(&w, &tm);
    char d[32]; strftime(d, sizeof d, "%d %b %H:%M", &tm);
    h.desc[sizeof h.desc - 1] = 0;
    snprintf(buf, n, "%s%s%s", d, h.desc[0] ? "  -  " : "", h.desc);
    return 1;
}
