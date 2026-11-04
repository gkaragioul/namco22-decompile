/* eng_state.h -- SAVE STATES (the Mac build's Esc > Save game / Load game): one pass over every chip's state, the same code for both
 * directions. A pass is SIZE (counts the bytes), SAVE (appends them to the buffer) or LOAD (reads them back, in the same order). A
 * structure holding pointers is copied whole and its pointer fields put back from the live copy afterwards (ST_KEEP), so a save never
 * carries an address from another run. Engine-side: engine/ss22_state.c drives the passes and the files. */
#ifndef ENG_STATE_H
#define ENG_STATE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    int      dir;                    /* 0 size, 1 save, 2 load */
    uint8_t *buf; size_t pos, cap;   /* SAVE: the growing image; LOAD: the image being read */
    int      bad;                    /* LOAD: the image ran out */
} eng_st;

static inline void st_io(eng_st *s, void *p, size_t n)
{
    if (s->dir == 0) { s->pos += n; return; }
    if (s->dir == 1) { if (s->pos + n <= s->cap) memcpy(s->buf + s->pos, p, n); else s->bad = 1; s->pos += n; return; }
    if (s->pos + n > s->cap) { s->bad = 1; return; }
    memcpy(p, s->buf + s->pos, n); s->pos += n;
}
#define ST(s, v) st_io((s), &(v), sizeof (v))
/* a structure with pointer fields: the whole thing, then (LOAD) each named field back from the live structure */
#define ST_KEEP_BEGIN(s, v) do { static __typeof__(v) st_live_; st_live_ = (v); ST((s), (v));   /* static: the DSP is ~800 KB */
#define ST_KEEP(v, f) (v).f = st_live_.f
#define ST_KEEP_END() } while (0)

/* each chip's pass (engine/<chip>.c) */
void rr_state(eng_st *s);            /* the 68K: registers, frame counter (engine/lift_cpu.c) */
void ss22_board_state(eng_st *s);    /* the board's RAMs and registers, not the ROM or the EEPROM (engine/ss22_board.c) */
void ss22_dsp_state(eng_st *s);      /* the master DSP (engine/ss22_dsp.c) */
void ss22_snd_state(eng_st *s);      /* the sound MCU, its timers, the C352 (engine/ss22_snd.c) */
void frame_rule_state(eng_st *s);    /* engine/frame_rule.c */
void c139_state(eng_st *s);          /* the link chip's registers (its RAM is the board's) */

/* the save / load controller (engine/ss22_state.c) */
#include <setjmp.h>
extern jmp_buf  g_ss22_resume_jb;    /* engine/ss22_run.c: a load comes back here */
extern uint32_t g_ss22_resume_pc;
extern int      g_ss22_state_req;    /* a request is pending: rr_tick calls ss22_state_poll */
extern int      g_ss22_state_test;   /* ENG_STATE_TEST (-1 not read yet) */
void ss22_state_poll(void);
void ss22_run_state(eng_st *s);
#endif
