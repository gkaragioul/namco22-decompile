/*
 * slave_list.c -- the SLAVE DSP's job: walk the master DSP's display list in
 * polygon RAM and hand every primitive to the geometry stage (geo_hw.c).
 *
 * One walker for both boards (namcos22_v.cpp simulate_slavedsp). Record
 * layout, from tools/pc_geo_fixed.py (byte-exact vs MAME):
 *   len 0x15  bb0003  viewport: zoom = dspfloat(src[6]),
 *                     view[row][col] = q15(src[0x0c + col*3 + row])
 *   len 0x10  233002  object shift / cz adjust (System 22: + object flags)
 *   len 0x0a  300000  view transform: view[row][col] = q15(src[1+col*3+row])
 *   len 0x0d  200002  primitive (code 0x5 or >= 0x45):
 *                     m[row][col] = q15(src[1+col*3+row])
 *                     t = sext24(src[0xa..0xc])
 *                     combined = (m . view) >> 15, t' = (t . view) >> 15
 * Moved here from src/framedump.c unchanged, apart from the board settings.
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "eng.h"
#include "geo_hw.h"
#include "slave_list.h"

extern int g_objlog_on;                            /* ENG_OBJLOG: geo_hw.c prints the quads while it is set (defined in geo_hw.c) */
static int32_t sext_n(uint32_t v, int bits)
{
    uint32_t m = 1u << (bits - 1);
    v &= (1u << bits) - 1u;
    return (int32_t)((v ^ m) - m);
}

static int32_t q15v(uint32_t v) { return sext_n(v & 0xffff, 16); }

/* namcos22_v.cpp:699 dspfloat, as a float (the clip window needs the real
 * value, not the mantissa/shift pair the projection uses). */
static float dspfloatf(uint32_t v)
{
    float mant = (float)sext_n(v & 0xffff, 16);
    int exp = (int)((v >> 16) & 0x3f);
    while (exp < 0x2e) { mant /= 2.0f; exp++; }
    return mant;
}

/* GeoFixed._reflectq: mirror the view by negating a COLUMN of the matrix. */
static void reflectq(int32_t m[3][3], int reflection)
{
    if (reflection & 0x10) for (int r = 0; r < 3; r++) m[r][0] = -m[r][0];
    if (reflection & 0x20) for (int r = 0; r < 3; r++) m[r][1] = -m[r][1];
}

int eng_walk_list(eng_word_fn pw, const eng_list_cfg *cfg, geo_quad_cb cb, void *user)
{
    /* ---- camera state, persistent across records ---- */
    int32_t viewq[3][3] = {{0x7FFF,0,0},{0,0x7FFF,0},{0,0,0x7FFF}};
    int have_view = 0;                 /* oracle: `if not hasattr(self,"viewq")` */
    int32_t zoom_mant = 0x7FFF; int zoom_shift = 15;
    int32_t vx = 0, vy = 0;
    int32_t absolute_priority = 0, objectshift = 0, cz_adjust = 0;
    int reflection = 0, cullflip = 0;
    int32_t amb_fx = 0, pow_fx = 0, light_fx[3] = {0, 0, 0};
    float cl = 0, cr = 0, cu = 0, cd = 0; int have_clip = 0;

    /* Fixed list head (simulate_slavedsp): Super 22 starts at 0x304, System 22
     * at 0x2FF. Deriving it from a pointer word and skipping an FFFE prologue
     * (what this once did) is not what the hardware does. */
    int src = cfg->head;
    int objectflags = 0;

    int guard = 0, prims = 0;
    while (guard++ < 4096) {
        uint32_t code = pw(src) & 0xFFFF;
        uint32_t len  = pw(src + 1) & 0xFFFF;
        const int p = src + 2;                        /* payload base */

        if (len == 0x15) {                            /* bb0003: viewport */
            absolute_priority = (int32_t)((pw(p + 3) >> 16) & 0xffff);
            vx = sext_n((pw(p + 5) >> 16) & 0xffff, 12);
            vy = sext_n( pw(p + 5)        & 0xffff, 12);
            uint32_t z = pw(p + 6);
            zoom_mant = sext_n(z & 0xffff, 16);
            { int e = (int)((z >> 16) & 0x3f); int sh = 0x2e - e;
              zoom_shift = sh < 0 ? 0 : sh; }
            amb_fx = (int32_t)((pw(p + 1) >> 16) & 0xffff);
            pow_fx = (int32_t)( pw(p + 1)        & 0xffff);
            for (int k = 0; k < 3; k++) light_fx[k] = q15v(pw(p + 2 + k));
            {   /* scene clip window, and the reflection swaps that go with it */
                float zf = dspfloatf(z);
                cl = dspfloatf(pw(p + 8)) * zf - 0.5f;   /* vl = src[8] */
                cr = dspfloatf(pw(p + 7)) * zf - 0.5f;   /* vr = src[7] */
                cu = dspfloatf(pw(p + 9)) * zf - 0.5f;
                cd = dspfloatf(pw(p + 10)) * zf - 0.5f;
            }
            reflection = (int)((pw(p + 2) >> 16) & 0x30);
            cullflip   = (reflection == 0x10 || reflection == 0x20);
            if (reflection & 0x10) { float t2 = cl; cl = cr; cr = t2; }
            if (reflection & 0x20) { float t2 = cu; cu = cd; cd = t2; }
            have_clip = 1;
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    viewq[row][col] = q15v(pw(p + 0x0c + col * 3 + row));
            reflectq(viewq, reflection);
            have_view = 1;
            cz_adjust = objectshift = 0;   /* viewport resets the shift group */
            objectflags = 0;
        } else if (len == 0x10) {                     /* 233002: object shift */
            cz_adjust   = (int32_t)(pw(p + 1) & 0xffffff);
            objectshift = (int32_t)(pw(p + 2) & 0xffffff);
            if (cfg->s22_objectflags) objectflags = (int)((pw(p + 3) >> 21) & 7);
        } else if (len == 0x0a) {                     /* 300000: view xform */
            for (int col = 0; col < 3; col++)
                for (int row = 0; row < 3; row++)
                    viewq[row][col] = q15v(pw(p + 1 + col * 3 + row));
            reflectq(viewq, reflection);
            have_view = 1;
        } else if (len == 0x0d) {                     /* 200002: primitive */
            if ((code == 0x5 || code >= 0x45) && have_view) {
                int32_t m[3][3], t[3];
                for (int col = 0; col < 3; col++)
                    for (int row = 0; row < 3; row++)
                        m[row][col] = q15v(pw(p + 1 + col * 3 + row));
                for (int k = 0; k < 3; k++)
                    t[k] = sext_n(pw(p + 0x0a + k), 24);

                geo_view gv;
                memset(&gv, 0, sizeof gv);
                for (int r = 0; r < 3; r++)
                    for (int c = 0; c < 3; c++)
                        gv.m[r][c] = (int32_t)(((int64_t)m[r][0] * viewq[0][c] +
                                                (int64_t)m[r][1] * viewq[1][c] +
                                                (int64_t)m[r][2] * viewq[2][c]) >> 15);
                for (int c = 0; c < 3; c++)
                    gv.t[c] = (int32_t)(((int64_t)t[0] * viewq[0][c] +
                                         (int64_t)t[1] * viewq[1][c] +
                                         (int64_t)t[2] * viewq[2][c]) >> 15);
                gv.zoom_mant = zoom_mant; gv.zoom_shift = zoom_shift;
                gv.vx = vx; gv.vy = vy;
                gv.objectshift = objectshift;
                gv.cz_adjust = cz_adjust;
                gv.cl = cl; gv.cr = cr; gv.cu = cu; gv.cd = cd;
                gv.have_clip = have_clip;
                gv.absolute_priority = absolute_priority;
                gv.cullflip = cullflip;
                memcpy(gv.viewq, viewq, sizeof viewq);
                gv.light[0] = light_fx[0]; gv.light[1] = light_fx[1];
                gv.light[2] = light_fx[2];
                gv.ambient = amb_fx; gv.power = pow_fx;
                gv.objectflags = objectflags;
                geo_hw_set_view(&gv);
                g_bbox_cur = (int)code;
                { static int olog = -2; if (olog == -2) { const char *e = getenv("ENG_OBJLOG"); olog = e ? (int)strtol(e, NULL, 0) : -1; }
                  if (olog == (int)code || olog == 0) {     /* ENG_OBJLOG=<code>: that model's record and every quad it gives; 0 = every model */
                      fprintf(stderr, "[OBJ] code %d t %d %d %d  ap %d objshift %06X cz %06X\n", (int)code, gv.t[0], gv.t[1], gv.t[2],
                              absolute_priority, (unsigned)objectshift & 0xFFFFFF, (unsigned)cz_adjust & 0xFFFFFF);
                      g_objlog_on = 1;
                  } }
                geo_hw_object((int32_t)code, cb, user);
                g_objlog_on = 0;
                g_bbox_cur = -1;
                objectflags &= ~2;             /* blit_polyobject: per object */
                prims++;
            }
        } else {
            break;                                    /* unknown length */
        }

        /* The next record must follow IMMEDIATELY; anything else ends the
         * list (oracle: `if nxt != index + length + 2: break`). */
        int nxt = (int)(pw(src + len + 3) & 0x7FFF);
        if (nxt != src + (int)len + 4) break;
        src = nxt;
    }
    if (cfg->out_zoom_mant) { *cfg->out_zoom_mant = zoom_mant; *cfg->out_zoom_shift = zoom_shift;
                              *cfg->out_vx = vx; *cfg->out_vy = vy; }
    return prims;
}

/* ---- MOTION SMOOTHING (the Mac build; Esc > Smooth motion) ----------------------------------------------------------------------
 * The board makes 59.906 pictures a second; at 120 Hz the window can show one more between two of them. It is made here, not by the
 * board: a copy of frame N's list whose records are moved part of the way back towards frame N-1 -- each viewport's and view
 * transform's camera matrix, each model's matrix and position -- then walked like any frame (as tc2/src/tc2_render.c does for Time
 * Crisis II). A record is matched with the same kind (a model: the same code) by OCCURRENCE: the k-th of a kind in frame N goes with
 * the k-th of that kind in frame N-1. Not moved: a record with no partner, one that jumped more than a third of its distance or turned
 * more than ~40 degrees in one frame (a new object in a reused slot). A cut -- the camera turned that far, under half the models found
 * a partner, or over a quarter of the partners jumped -- gives frame N itself. Not the board's picture: nothing here is checked
 * against MAME. */
typedef struct { uint32_t key; int seq, p, len; } mid_rec;
static int mid_cmp(const void *x, const void *y)
{
    const mid_rec *a = x, *b = y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->seq - b->seq;
}
/* the records of one list, in list order: key = kind (1 viewport, 2 view transform, 3 << 16 | code a model), seq = its occurrence */
static int mid_records(const uint32_t *w, int head, mid_rec *out, int max)
{
    int src = head, n = 0, guard = 0;
    while (guard++ < 4096 && n < max) {
        const uint32_t code = w[src & 0x7FFF] & 0xFFFF, len = w[(src + 1) & 0x7FFF] & 0xFFFF;
        uint32_t key = 0;
        if (len == 0x15) key = 1;
        else if (len == 0x0a) key = 2;
        else if (len == 0x0d) { if (code == 0x5 || code >= 0x45) key = 3u << 16 | code; }
        else if (len != 0x10) break;
        if (key) { out[n].key = key; out[n].p = src + 2; out[n].len = (int)len; out[n].seq = 0; n++; }
        const int nxt = (int)(w[(src + len + 3) & 0x7FFF] & 0x7FFF);
        if (nxt != src + (int)len + 4) break;
        src = nxt;
    }
    return n;
}
static double mid_len(const double v[3]) { return sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
/* a Q15 3x3 at w[base + col*3 + row] (a's at abase, b's and o's at base): b's moved a fraction f back towards a, into o.
 * 0 = turned too far (o untouched) */
static int mid_matrix(const uint32_t *a, int abase, const uint32_t *b, uint32_t *o, int base, double f)
{
    double m[3][3];
    for (int r = 0; r < 3; r++) {
        double va[3], vb[3];
        for (int c = 0; c < 3; c++) { va[c] = q15v(a[(abase + c * 3 + r) & 0x7FFF]); vb[c] = q15v(b[(base + c * 3 + r) & 0x7FFF]); }
        const double la = mid_len(va), lb = mid_len(vb);
        if (la < 1 || lb < 1) { for (int c = 0; c < 3; c++) m[r][c] = vb[c]; continue; }
        if ((va[0] * vb[0] + va[1] * vb[1] + va[2] * vb[2]) / (la * lb) < 0.766) return 0;   /* turned more than ~40 degrees */
        double v[3]; for (int c = 0; c < 3; c++) v[c] = vb[c] + (va[c] - vb[c]) * f;
        const double l = mid_len(v), want = lb + (la - lb) * f;
        for (int c = 0; c < 3; c++) m[r][c] = l > 0 ? v[c] * want / l : vb[c];
    }
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        const int i = (base + c * 3 + r) & 0x7FFF;
        long q = lround(m[r][c]); if (q > 32767) q = 32767; if (q < -32768) q = -32768;
        o[i] = (b[i] & 0xFFFF0000u) | ((uint32_t)q & 0xFFFFu);
    }
    return 1;
}
int eng_list_mid(const uint32_t *a, const uint32_t *b, uint32_t *out, int head, double f)
{
    enum { MAXR = 4096 };
    static mid_rec ra[MAXR], rb[MAXR];
    memcpy(out, b, sizeof(uint32_t) * 0x8000);
    const int na = mid_records(a, head, ra, MAXR), nb = mid_records(b, head, rb, MAXR);
    if (!na || !nb) return 0;
    /* occurrence numbers: count each key in list order (a small open-addressed table of counters) */
    for (int pass = 0; pass < 2; pass++) {
        mid_rec *r = pass ? rb : ra; const int n = pass ? nb : na;
        static uint32_t hk[8192]; static int hc[8192];
        memset(hk, 0, sizeof hk);
        for (int i = 0; i < n; i++) {
            unsigned h = (r[i].key * 2654435761u) >> 19;
            while (hk[h] && hk[h] != r[i].key) h = (h + 1) & 8191;
            if (!hk[h]) { hk[h] = r[i].key; hc[h] = 0; }
            r[i].seq = hc[h]++;
        }
    }
    qsort(ra, (size_t)na, sizeof ra[0], mid_cmp);
    int models = 0, matched = 0, jumped = 0;
    for (int i = 0; i < nb; i++) {
        const mid_rec *pb = &rb[i];
        if (pb->key >> 16 == 3) models++;
        const mid_rec *pa = bsearch(pb, ra, (size_t)na, sizeof ra[0], mid_cmp);
        if (!pa) continue;
        if (pb->key == 1 || pb->key == 2) {                   /* the camera */
            const int off = pb->key == 1 ? 0x0c : 1;
            if (!mid_matrix(a, pa->p + off, b, out, pb->p + off, f)) { memcpy(out, b, sizeof(uint32_t) * 0x8000); return 0; }
            continue;
        }
        matched++;
        /* the model's position: sext24 at p + 0x0a..0x0c */
        double ta[3], tb[3], d[3];
        for (int k = 0; k < 3; k++) {
            ta[k] = sext_n(a[(pa->p + 0x0a + k) & 0x7FFF], 24); tb[k] = sext_n(b[(pb->p + 0x0a + k) & 0x7FFF], 24); d[k] = tb[k] - ta[k];
        }
        const double dist = mid_len(d), r = mid_len(ta) > mid_len(tb) ? mid_len(ta) : mid_len(tb);
        if (dist > 64.0 && dist > r / 3.0) { jumped++; continue; }
        /* the matrix first (it can refuse: a turn too far), then the position */
        uint32_t keep[9];
        for (int k = 0; k < 9; k++) keep[k] = out[(pb->p + 1 + k) & 0x7FFF];
        if (!mid_matrix(a, pa->p + 1, b, out, pb->p + 1, f)) { for (int k = 0; k < 9; k++) out[(pb->p + 1 + k) & 0x7FFF] = keep[k]; jumped++; continue; }
        for (int k = 0; k < 3; k++) {
            const int idx = (pb->p + 0x0a + k) & 0x7FFF;
            const long v = lround(tb[k] + (ta[k] - tb[k]) * f);
            out[idx] = (b[idx] & 0xFF000000u) | ((uint32_t)v & 0xFFFFFFu);
        }
    }
    if (models && (matched * 2 < models || jumped * 4 > matched)) { memcpy(out, b, sizeof(uint32_t) * 0x8000); return 0; }
    return 1;
}
