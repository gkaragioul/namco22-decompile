/* tc2_hd.c -- HD TEXTURES for Time Crisis II (the Mac build), with Time Crisis 1's machinery (macos/tex_hd.h: the hash, the HD folder and
 * its in-memory preload, the collection of textures that have no HD image yet, the upscaler at quit).
 *
 * The board -- and src/tc2_gl.c -- never bakes a texture: the shader looks up tile map -> tile -> pen -> palette for every pixel. So here,
 * on the frame builder's thread (src/tc2_render.c build), each textured polygon's UV box is baked ONCE in colour exactly as the shader
 * would colour it (the tile map, the colour mode's pen bits, the palette at color_base), and that image goes through tex_hd_after_bake:
 * an HD image by its hash, or a PNG for the upscaler. A polygon with an HD image carries its place in the HD atlas (tc2_poly.hd: atlas x,
 * y, the box's u0, v0, the scale) and the shader samples the atlas, bilinear, instead of the palette; the stencil, shading, fades and alpha
 * (by pen too: the pen is still looked up) are the board's as before.
 *
 * Kept per UV box, colour mode, palette base and that palette's generation (a group of 256 pens changed = a new generation), so a palette
 * effect is simply another image, as on Time Crisis 1. At most BAKE_MS of new bakes a built frame (a new scene arrives over a few frames,
 * never as a stall). The atlas is one 8192 x 8192 texture, filled in shelves with a one-texel guard; full, it starts again (every polygon
 * baked again). Display > HD textures off: no HD polygons (the cache is dropped). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include "tc2_build.h"

static atomic_int hd_flush;                                   /* HD on / off, or a new pack: drop the cache and the atlas (the builder thread does it) */
#define TEX_HD_CHANGED() atomic_store(&hd_flush, 1)
#include "../../macos/tex_hd.h"

#define ATLAS_W 16384                                         /* one 16384 x 8192 atlas: a scene's images at 2x fill most of 8192 x 8192 */
#define ATLAS_H 8192
#define CACHE_N 65536                                         /* open addressing, a power of two */
#define BAKE_MS 4.0
#define BOX_MAX 1024                                          /* a UV box wider or taller than this stays native (a repeating floor) */

typedef struct { uint64_t key; int st; uint64_t wait; float ax, ay; int sc; } ent;   /* st: 1 no HD, 2 HD in the atlas, 3 decoding */
static ent cache[CACHE_N]; static int cache_n;
static uint32_t pal_gen[128]; static uint32_t prev_pens[0x8000]; static int have_prev;
/* the atlas packer (builder thread): a SKYLINE, bottom-left -- the top edge of what is placed, as segments (x, y, w) left to right; an image
 * goes where its bottom is lowest. Rows of one height (the first packers) filled at 25-40 %; this packs mixed sizes to ~80 % */
#define SKY_MAX 4096
static struct { int x, y, w; } sky[SKY_MAX]; static int sky_n;
static void sky_reset(void) { sky_n = 1; sky[0].x = 1; sky[0].y = 1; sky[0].w = ATLAS_W - 2; }
static int sky_fit(int i, int W, int *y)                      /* the lowest y an image W wide can sit at, starting at segment i; -1 = no room */
{
    const int x = sky[i].x; if (x + W > ATLAS_W - 1) return -1;
    int left = W, yy = 0;
    for (int j = i; left > 0; j++) { if (j >= sky_n) return -1; if (sky[j].y > yy) yy = sky[j].y; left -= sky[j].w; }
    *y = yy; return 0;
}
static int sky_put(int W, int H, int *ox, int *oy)
{
    int best = -1, by = 0x7FFFFFFF, bx = 0;
    for (int i = 0; i < sky_n; i++) { int y; if (sky_fit(i, W, &y) == 0 && y + H <= ATLAS_H - 1 && (y < by || (y == by && sky[i].x < bx))) { best = i; by = y; bx = sky[i].x; } }
    if (best < 0 || sky_n + 2 >= SKY_MAX) return 0;
    /* the new segment [bx, bx + W) at by + H replaces what it covers */
    const int x1 = bx + W;
    int j = best;
    while (j < sky_n && sky[j].x + sky[j].w <= x1) j++;          /* segments wholly covered: best .. j-1 */
    int keep_from = j;
    if (j < sky_n && sky[j].x < x1) { const int cut = x1 - sky[j].x; sky[j].x += cut; sky[j].w -= cut; }   /* the one partly covered */
    const int removed = keep_from - best;
    memmove(&sky[best + 1], &sky[keep_from], (size_t)(sky_n - keep_from) * sizeof sky[0]);
    sky_n = sky_n - removed + 1;
    sky[best].x = bx; sky[best].y = by + H; sky[best].w = W;
    for (int k = 0; k + 1 < sky_n; ) if (sky[k].y == sky[k + 1].y) { sky[k].w += sky[k + 1].w; memmove(&sky[k + 1], &sky[k + 2], (size_t)(sky_n - k - 2) * sizeof sky[0]); sky_n--; } else k++;
    *ox = bx; *oy = by;
    return 1;
}

/* the uploads the GL thread does before it draws (src/tc2_gl.c tc2_hd_uploads) */
typedef struct up { struct up *next; int x, y, w, h; uint8_t px[]; } up;
static up *q_head, *q_tail; static int q_reset; static pthread_mutex_t q_mu = PTHREAD_MUTEX_INITIALIZER;

static unsigned long n_resets; static double used_texels;
static void atlas_reset(void)
{
    if (cache_n) { n_resets++; fprintf(stderr, "[TEX-HD] TC2 atlas restarted (%lu so far): %d textures, %.0f%% of it in images\n", n_resets, cache_n, 100.0 * used_texels / ((double)ATLAS_W * ATLAS_H)); }
    used_texels = 0;
    memset(cache, 0, sizeof cache); cache_n = 0;
    sky_reset();
    pthread_mutex_lock(&q_mu);
    while (q_head) { up *n = q_head->next; free(q_head); q_head = n; }
    q_tail = NULL; q_reset = 1;
    pthread_mutex_unlock(&q_mu);
}
/* a w x h image (stride w * 4) into the atlas with a one-texel edge copy all round; 0 = no room */
static int atlas_put(const uint8_t *px, int w, int h, int *ax, int *ay)
{
    const int W = w + 2, H = h + 2;
    if (W > ATLAS_W || H > ATLAS_H) return 0;
    if (!sky_n) sky_reset();
    int sx, sy;
    if (!sky_put(W, H, &sx, &sy)) return 0;
    *ax = sx + 1; *ay = sy + 1;
    up *u = malloc(sizeof *u + (size_t)W * H * 4); if (!u) return 0;
    u->x = sx; u->y = sy; u->w = W; u->h = H; u->next = NULL;
    for (int y = 0; y < H; y++) {
        const int sy = y == 0 ? 0 : y == H - 1 ? h - 1 : y - 1;
        uint8_t *d = u->px + (size_t)y * W * 4; const uint8_t *s = px + (size_t)sy * w * 4;
        memcpy(d + 4, s, (size_t)w * 4); memcpy(d, s, 4); memcpy(d + (size_t)(W - 1) * 4, s + (size_t)(w - 1) * 4, 4);
    }
    used_texels += (double)W * H;
    pthread_mutex_lock(&q_mu);
    if (q_tail) q_tail->next = u; else q_head = u;
    q_tail = u;
    pthread_mutex_unlock(&q_mu);
    return 1;
}
/* the GL thread, before a frame's polygons are drawn: every pending upload, through put(x, y, w, h, rgba); 1 = the atlas was restarted */
int tc2_hd_uploads(void (*put)(int x, int y, int w, int h, const uint8_t *px))
{
    pthread_mutex_lock(&q_mu);
    up *l = q_head; q_head = q_tail = NULL; const int r = q_reset; q_reset = 0;
    pthread_mutex_unlock(&q_mu);
    while (l) { up *n = l->next; put(l->x, l->y, l->w, l->h, l->px); free(l); l = n; }
    return r;
}

static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h * 0xFF51AFD7ED558CCDull; }
static ent *find(uint64_t key, int add)
{
    unsigned i = (unsigned)(key ^ key >> 29) & (CACHE_N - 1);
    for (int p = 0; p < 64; p++, i = (i + 1) & (CACHE_N - 1)) {
        if (cache[i].key == key && cache[i].st) return &cache[i];
        if (!cache[i].st) { if (!add) return NULL; cache[i].key = key; cache_n++; return &cache[i]; }
    }
    return NULL;
}

/* the frame's polygons: an HD place for each textured one that has an image (builder thread, src/tc2_render.c build) */
unsigned g_eng_frame;                                         /* macos/tex_hd.h's per-frame decode budget counts these: one per built frame */
void tc2_hd_assign(tc2_built *b, const uint8_t *texrom, const uint32_t *tm, const uint8_t *tattr)
{
    g_eng_frame++;
    for (int i = 0; i < b->npoly; i++) b->polys[i].hd[4] = 0;
    if (!tex_hd_enabled() || !texrom) return;
    if (atomic_exchange(&hd_flush, 0) || cache_n > CACHE_N * 3 / 4) atlas_reset();
    /* the palette's generations: a group of 256 pens that changed since the last frame is new art */
    const uint32_t *pens = b->fs.pens;
    if (!have_prev) { memcpy(prev_pens, pens, sizeof prev_pens); have_prev = 1; }
    static unsigned long n_bakes, n_palchg, n_frames;
    for (int g = 0; g < 128; g++) if (memcmp(prev_pens + g * 256, pens + g * 256, 256 * 4)) { pal_gen[g]++; n_palchg++; memcpy(prev_pens + g * 256, pens + g * 256, 256 * 4); }
    if (++n_frames % 600 == 0) { fprintf(stderr, "[TEX-HD] TC2 last 600 built frames: %lu textures baked, %lu palette groups changed, %d cached\n", n_bakes, n_palchg, cache_n); n_bakes = n_palchg = 0; }
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); double spent = 0;
    static uint8_t *bake; static size_t bake_cap;
    for (int i = 0; i < b->npoly; i++) {
        tc2_poly *p = &b->polys[i];
        if (p->n < 3) continue;
        const float u0 = p->uvbox[0], v0 = p->uvbox[1], u1 = p->uvbox[2], v1 = p->uvbox[3];   /* before the near clip: stable */
        if (!(u1 >= u0 && v1 >= v0) || u0 < -1e6f || u1 > 1e6f || v0 < -1e6f || v1 > 1e6f) continue;
        const int iu0 = (int)floorf(u0), iv0 = (int)floorf(v0), w = (int)floorf(u1) - iu0 + 1, h = (int)floorf(v1) - iv0 + 1;
        if (w < 2 || h < 2 || w * h < 64 || w > BOX_MAX || h > BOX_MAX) continue;
        const uint32_t pb = (uint32_t)p->color_base;
        uint64_t key = mix64(mix64(mix64(0x7C2A11, (uint64_t)(uint32_t)iu0 << 32 | (uint32_t)iv0), (uint64_t)w << 32 | (uint32_t)h),
                             (uint64_t)(uint32_t)p->tbase << 32 | (uint64_t)(p->cmode & 0xF) << 24 | (pb & 0xFFFFFF));
        key = mix64(key, (uint64_t)pal_gen[(pb >> 8) & 127] << 32 | pal_gen[((pb + 255) >> 8) & 127]);
        if (!key) key = 1;
        ent *e = find(key, 0);
        if (!e || (e->st == 3 && tex_hd_ready(e->wait))) {    /* new, or its HD image is decoded now: bake (again) */
            if (spent >= BAKE_MS) continue;                   /* the frame's budget is spent: native this time */
            if ((size_t)w * h * 4 > bake_cap) { bake_cap = (size_t)w * h * 4; free(bake); bake = malloc(bake_cap); if (!bake) { bake_cap = 0; continue; } }
            /* the box in colour, as the shader colours it (src/tc2_gl.c fs_poly; src/tc2_render.c tex_lookup) */
            uint32_t pbase = pb; int pmask = 0xFF, pshift = 0;
            if (p->cmode & 4) { pbase += 0xEC + ((p->cmode & 8) << 1); pmask = 3; pshift = 2 * (~p->cmode & 3); }
            else if (p->cmode & 2) { pbase += 0xE0 + ((p->cmode & 8) << 1); pmask = 0xF; pshift = 4 * (~p->cmode & 1); }
            uint8_t *o = bake;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                uint32_t u = (uint32_t)(iu0 + x), v = (uint32_t)(iv0 + y) + (uint32_t)p->tbase;
                const uint32_t id = ((u >> 4) & 0xFF) | ((v << 4) & 0x1FFF00u);
                const uint32_t tile = tm[id], a = tattr[id];
                if (a & 1) v = ~v;
                if (a & 2) u = ~u;
                if (a & 4) { const uint32_t t = u; u = v; v = t; }
                const uint8_t pen = texrom[(tile | ((v << 4) & 0xF0) | (u & 0x0F)) & 0x1FFFFFF];
                const uint32_t c = pens[(pbase + ((uint32_t)(pen >> pshift) & (uint32_t)pmask)) & 0x7FFF];
                *o++ = (uint8_t)(c >> 16); *o++ = (uint8_t)(c >> 8); *o++ = (uint8_t)c; *o++ = 0xFF;
            }
            int hw = 0, hh = 0; uint64_t wait = 0;
            const uint8_t *hd = tex_hd_after_bake(bake, w * 4, w, h, &hw, &hh, &wait);
            n_bakes++;
            if (!e) e = find(key, 1);
            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            spent = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;
            if (!e) continue;
            e->st = 1;
            if (wait) { e->st = 3; e->wait = wait; }
            else if (hd) {
                int ax, ay;
                if (!atlas_put(hd, hw, hh, &ax, &ay)) { atomic_store(&hd_flush, 1); e->st = 1; }   /* full: start again next frame */
                else { e->st = 2; e->ax = (float)ax; e->ay = (float)ay; e->sc = hw / w; }
            }
        }
        if (e->st == 2) { p->hd[0] = e->ax; p->hd[1] = e->ay; p->hd[2] = (float)iu0; p->hd[3] = (float)iv0; p->hd[4] = (float)e->sc; }
    }
}
/* at quit (src/tc2_window.c): what this session collected goes to the upscaler */
void tc2_hd_quit(void) { tex_hd_quit(); }
