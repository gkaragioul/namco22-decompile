/* slave_list.h -- walk the master DSP's display list (see slave_list.c). */
#ifndef ENG_SLAVE_LIST_H
#define ENG_SLAVE_LIST_H
#include <stdint.h>
#include "geo_hw.h"

/* One polygon-RAM word (24-bit value, sign-extended or not), index & 0x7FFF. */
typedef uint32_t (*eng_word_fn)(int index);

typedef struct {
    int head;               /* ENG_LIST_HEAD_SS22 (0x304) or ENG_LIST_HEAD_S22 (0x2FF) */
    int s22_objectflags;    /* System 22: the 0x10 record carries object flags */
    /* optional: the last viewport's projection, for callers that report it */
    int32_t *out_zoom_mant; int *out_zoom_shift; int32_t *out_vx, *out_vy;
} eng_list_cfg;
#define ENG_LIST_HEAD_SS22 0x304
#define ENG_LIST_HEAD_S22  0x2FF

/* Returns the number of primitives walked. */
int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user);

/* MOTION SMOOTHING: the in-between list of two frames' polygon RAM (a = frame N-1, b = frame N, 0x8000 words each) into out,
 * a copy of b whose camera and model matrices / positions are moved a fraction f of the way back from b towards a (see
 * slave_list.c). 1 = moved, 0 = a cut (out is b as it is). */
int eng_list_mid(const uint32_t *a, const uint32_t *b, uint32_t *out, int head, double f);
#endif
