/* macos/tex_hd.h -- HD texture replacement for the System 22 texture bake (engine/tex_bake.c), the Mac build.
 *
 * A baked texture is the RGBA image of one quad's UV box, through its palette, at the board's own texel density. It is
 * identified by a 64-bit FNV-1a hash of exactly those pixels (size included), so the same art through the same colours is
 * the same file wherever and whenever it is baked; a palette effect (a flash, a fade) is simply another image.
 *   ENG_TEX_HD=<dir>    <dir>/<hash>.png (16 hex digits), a whole multiple of the original size, replaces that bake
 *   ENG_TEX_DUMP=<dir>  every native bake that has no HD image yet, once, as <dir>/<hash>.png: what to upscale next
 *                       (the app's launcher sets both; at quit the game starts the upscaler on what it collected)
 * The HD folder is indexed once (no file access per bake) and the whole pack is decoded into memory in the background at 2x (the
 * preload, ~400 MB); until then images are decoded on demand and kept up to 512 MB, least recently
 * used first out. Display > HD textures (hd_textures= in the cfg, on by default) turns it off and on, re-baking everything.
 * Only native-density bakes (step 1) take part; an HD image over 2048 texels a side is not used (the atlas page is 4096). */
#ifndef TEX_HD_H
#define TEX_HD_H
#include <zlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#ifndef TEX_HD_CHANGED                                          /* what HD on / off re-does: Time Crisis 1 re-bakes every texture (its cache keys on the
                                                               * palette generation); Time Crisis II defines its own (tc2/src/tc2_hd.c) */
#include "eng.h"
#define TEX_HD_CHANGED() do { for (int i_ = 0; i_ < ENG_PAL_GROUPS; i_++) g_eng_pal_gen[i_]++; } while (0)
#endif

int g_tex_hd_on = -1;                                         /* -1: not read from the cfg yet */
unsigned g_tex_hd_dumped;                                     /* new textures collected this session (the quit hook upscales them) */
static const char *tex_hd_dump_dir(void) { static int i; static const char *d; if (!i) { i = 1; d = getenv("ENG_TEX_DUMP"); } return d; }
static const char *tex_hd_dir(void) { static int i; static const char *d; if (!i) { i = 1; d = getenv("ENG_TEX_HD"); } return d; }
extern int eng_cfg_int(const char *key, int def);
static int tex_hd_enabled(void) { if (g_tex_hd_on < 0) g_tex_hd_on = eng_cfg_int("hd_textures", 1) != 0; return g_tex_hd_on && tex_hd_dir(); }
/* the menu's switch: every cached texture is re-baked (the cache keys on the palette generation) */
void tex_hd_set(int on) { g_tex_hd_on = on != 0; TEX_HD_CHANGED(); }
int  tex_hd_available(void) { return tex_hd_dir() != NULL; }
int  tex_hd_is_on(void) { tex_hd_enabled(); return g_tex_hd_on; }

static uint64_t tex_hd_hash(const uint8_t *px, int stride, int w, int h)
{
    uint64_t x = 1469598103934665603ull;
    x ^= (uint64_t)w; x *= 1099511628211ull; x ^= (uint64_t)h; x *= 1099511628211ull;
    for (int y = 0; y < h; y++) {
        const uint8_t *r = px + (size_t)y * stride;
        for (int i = 0; i < w * 4; i++) { x ^= r[i]; x *= 1099511628211ull; }
    }
    return x;
}

static void tex_hd_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void tex_hd_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t b[8]; tex_hd_be32(b, n); memcpy(b + 4, type, 4); fwrite(b, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uLong c = crc32(0, (const Bytef *)type, 4); c = crc32(c, data, n); tex_hd_be32(b, (uint32_t)c); fwrite(b, 1, 4, f);
}
static void tex_hd_write_png(const char *path, const uint8_t *px, int stride, int w, int h)
{
    const size_t raw_n = (size_t)h * (w * 4 + 1);
    uint8_t *raw = malloc(raw_n); if (!raw) return;
    for (int y = 0; y < h; y++) { raw[(size_t)y * (w * 4 + 1)] = 0; memcpy(raw + (size_t)y * (w * 4 + 1) + 1, px + (size_t)y * stride, (size_t)w * 4); }
    uLongf zn = compressBound(raw_n); uint8_t *z = malloc(zn);
    if (z && compress2(z, &zn, raw, raw_n, 6) == Z_OK) {
        char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.tmp", path);   /* whole files only: the upscaler may be reading the folder */
        FILE *f = fopen(tmp, "wb");
        if (f) {
            fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
            uint8_t ih[13]; tex_hd_be32(ih, (uint32_t)w); tex_hd_be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 6; ih[10] = ih[11] = ih[12] = 0;
            tex_hd_chunk(f, "IHDR", ih, 13); tex_hd_chunk(f, "IDAT", z, (uint32_t)zn); tex_hd_chunk(f, "IEND", NULL, 0);
            if (fclose(f) == 0) rename(tmp, path); else remove(tmp);
        }
    }
    free(z); free(raw);
}
/* collected textures are written by a thread of their own: a PNG compress per new texture in the frame was a missed vsync */
typedef struct hd_job { struct hd_job *next; char path[1100]; int w, h; uint8_t px[]; } hd_job;
static hd_job *hd_q_head, *hd_q_tail; static int hd_q_n; static pthread_mutex_t hd_q_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hd_q_cv = PTHREAD_COND_INITIALIZER; static int hd_q_started;
static void *hd_writer(void *u)
{
    (void)u;
    for (;;) {
        pthread_mutex_lock(&hd_q_mu);
        while (!hd_q_head) pthread_cond_wait(&hd_q_cv, &hd_q_mu);
        hd_job *j = hd_q_head; hd_q_head = j->next; if (!hd_q_head) hd_q_tail = NULL; hd_q_n--;
        pthread_mutex_unlock(&hd_q_mu);
        tex_hd_write_png(j->path, j->px, j->w * 4, j->w, j->h);
        free(j);
    }
    return NULL;
}
static void hd_queue_write(const char *path, const uint8_t *px, int stride, int w, int h)
{
    if (hd_q_n > 8192) return;                                /* far behind: this one waits for another session */
    hd_job *j = malloc(sizeof *j + (size_t)w * h * 4); if (!j) return;
    snprintf(j->path, sizeof j->path, "%s", path); j->w = w; j->h = h; j->next = NULL;
    for (int y = 0; y < h; y++) memcpy(j->px + (size_t)y * w * 4, px + (size_t)y * stride, (size_t)w * 4);
    pthread_mutex_lock(&hd_q_mu);
    if (!hd_q_started) { pthread_t t; if (pthread_create(&t, NULL, hd_writer, NULL) == 0) { pthread_detach(t); hd_q_started = 1; } }
    if (hd_q_tail) hd_q_tail->next = j; else hd_q_head = j;
    hd_q_tail = j; hd_q_n++;
    pthread_cond_signal(&hd_q_cv);
    pthread_mutex_unlock(&hd_q_mu);
}
static void hd_queue_drain(void)                              /* at quit: what is still queued is written first */
{
    for (int i = 0; i < 400; i++) {                           /* at most ~4 s */
        pthread_mutex_lock(&hd_q_mu); const int n = hd_q_n; pthread_mutex_unlock(&hd_q_mu);
        if (!n) break;
        struct timespec ts = { 0, 10000000 }; nanosleep(&ts, NULL);
    }
}

/* an 8-bit RGB or RGBA, non-interlaced PNG -> malloc'd RGBA (the upscaler's output); NULL if not that */
static uint8_t *tex_hd_read_png(const char *path, int *ow, int *oh)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 8 ? malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (memcmp(d, "\x89PNG\r\n\x1a\n", 8)) { free(d); return NULL; }
    int w = 0, h = 0, ct = -1; uint8_t *z = NULL; size_t zn = 0, zc = 0; long p = 8;
    while (p + 12 <= n) {
        const uint32_t len = (uint32_t)d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3];
        const uint8_t *t = d + p + 4, *c = d + p + 8;
        if (p + 12 + (long)len > n) break;
        if (!memcmp(t, "IHDR", 4) && len >= 13) {
            w = (int)((uint32_t)c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3]); h = (int)((uint32_t)c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
            if (c[8] != 8 || (c[9] != 2 && c[9] != 6) || c[12] != 0) { free(d); return NULL; }
            ct = c[9];
        } else if (!memcmp(t, "IDAT", 4)) {
            if (zn + len > zc) { zc = (zn + len) * 2; uint8_t *nz = realloc(z, zc); if (!nz) { free(z); free(d); return NULL; } z = nz; }
            memcpy(z + zn, c, len); zn += len;
        } else if (!memcmp(t, "IEND", 4)) break;
        p += 12 + (long)len;
    }
    free(d);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || ct < 0 || !z) { free(z); return NULL; }
    const int bpp = ct == 6 ? 4 : 3, rowb = w * bpp;
    uLongf rn = (uLongf)h * (rowb + 1); uint8_t *raw = malloc(rn), *out = malloc((size_t)w * h * 4);
    if (!raw || !out || uncompress(raw, &rn, z, zn) != Z_OK || rn != (uLongf)h * (rowb + 1)) { free(z); free(raw); free(out); return NULL; }
    free(z);
    uint8_t *prev = NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + (size_t)y * (rowb + 1) + 1; const int ft = r[-1];
        for (int i = 0; i < rowb; i++) {
            const int a = i >= bpp ? r[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = r[i];
            if (ft == 1) v += a; else if (ft == 2) v += b; else if (ft == 3) v += (a + b) / 2;
            else if (ft == 4) { const int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c); v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c; }
            r[i] = (uint8_t)v;
        }
        for (int x = 0; x < w; x++) {
            uint8_t *o = out + ((size_t)y * w + x) * 4;
            o[0] = r[x * bpp]; o[1] = r[x * bpp + 1]; o[2] = r[x * bpp + 2]; o[3] = bpp == 4 ? r[x * bpp + 3] : 255;
        }
        prev = r;
    }
    free(raw);
    *ow = w; *oh = h;
    return out;
}

/* ---- the HD folder's index (sorted hashes), read once ---- */
static uint64_t *hd_idx; static size_t hd_n;
static int hd_cmp(const void *a, const void *b) { const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static void hd_index(void)
{
    static int done; if (done) return; done = 1;
    DIR *d = opendir(tex_hd_dir()); if (!d) return;
    size_t cap = 0;
    for (struct dirent *e; (e = readdir(d)); ) {
        if (strlen(e->d_name) != 20 || strcmp(e->d_name + 16, ".png")) continue;
        char *end; const uint64_t k = strtoull(e->d_name, &end, 16); if (end != e->d_name + 16) continue;
        if (hd_n == cap) { cap = cap ? cap * 2 : 4096; uint64_t *n2 = realloc(hd_idx, cap * sizeof *n2); if (!n2) break; hd_idx = n2; }
        hd_idx[hd_n++] = k;
    }
    closedir(d);
    qsort(hd_idx, hd_n, sizeof *hd_idx, hd_cmp);
    fprintf(stderr, "[TEX-HD] %zu HD textures in %s\n", hd_n, tex_hd_dir());
}

/* ---- decoded images, least recently used out past 512 MB ---- */
#define HD_SLOTS 8192
static struct { uint64_t k; uint8_t *px; int w, h; unsigned long use; } hd_cache[HD_SLOTS];
static size_t hd_bytes; static unsigned long hd_clock;
static const uint8_t *hd_cache_find(uint64_t k, int *w, int *h)
{
    unsigned i = (unsigned)(k ^ (k >> 32)) % HD_SLOTS;
    for (int p = 0; p < 16; p++, i = (i + 1) % HD_SLOTS)
        if (hd_cache[i].px && hd_cache[i].k == k) { hd_cache[i].use = ++hd_clock; *w = hd_cache[i].w; *h = hd_cache[i].h; return hd_cache[i].px; }
    return NULL;
}
static const uint8_t *hd_cache_put(uint64_t k, uint8_t *px, int W, int H)
{
    const size_t need = (size_t)W * H * 4;
    while (hd_bytes + need > ((size_t)512 << 20)) {         /* evict the least recently used */
        int lru = -1; for (int j = 0; j < HD_SLOTS; j++) if (hd_cache[j].px && (lru < 0 || hd_cache[j].use < hd_cache[lru].use)) lru = j;
        if (lru < 0) break;
        hd_bytes -= (size_t)hd_cache[lru].w * hd_cache[lru].h * 4; free(hd_cache[lru].px); hd_cache[lru].px = NULL;
    }
    unsigned i = (unsigned)(k ^ (k >> 32)) % HD_SLOTS;
    int slot = -1; for (int p = 0; p < 16; p++, i = (i + 1) % HD_SLOTS) if (!hd_cache[i].px) { slot = (int)i; break; }
    if (slot < 0) {                                           /* the probe window is full: replace its oldest */
        i = (unsigned)(k ^ (k >> 32)) % HD_SLOTS; slot = (int)i;
        for (int p = 1; p < 16; p++) { const unsigned j = (i + p) % HD_SLOTS; if (hd_cache[j].use < hd_cache[slot].use) slot = (int)j; }
        hd_bytes -= (size_t)hd_cache[slot].w * hd_cache[slot].h * 4; free(hd_cache[slot].px);
    }
    hd_cache[slot].k = k; hd_cache[slot].px = px; hd_cache[slot].w = W; hd_cache[slot].h = H; hd_cache[slot].use = ++hd_clock;
    hd_bytes += need;
    return px;
}

/* ---- PNG decoding on worker threads: decoding a new scene's images in the frame stalled it 0.1-0.4 s. A texture seen for
 * the first time draws native for a few frames; its bake is redone once the HD image is ready (tex_hd_ready). ---- */
#define HD_PEND 1024
#define HD_MAX_SCALE 2                                     /* times the native size, at most (the decoder halves 4x images) */
#define HD_PEND_WIN 32
static struct { uint64_t k; uint8_t *px; int w, h, nw, nh, st; } hd_pend[HD_PEND];   /* st: 0 free, 1 queued, 2 decoding, 3 done; nw x nh the native size */
static pthread_mutex_t hd_pmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t hd_pcv = PTHREAD_COND_INITIALIZER;
static int hd_pqueued, hd_pworkers;
static int hd_preloading;                                     /* the whole pack is being decoded (below): no decoding in the frame at all */
unsigned long g_tex_hd_waits, g_tex_hd_redone;                /* bakes drawn native while decoding; of them, redone sharp (logs) */
static int hd_pfind(uint64_t k)                               /* (under hd_pmu) */
{
    unsigned i = (unsigned)(k ^ (k >> 32)) % HD_PEND;
    for (int p = 0; p < HD_PEND_WIN; p++, i = (i + 1) % HD_PEND) if (hd_pend[i].st && hd_pend[i].k == k) return (int)i;
    return -1;
}
/* k's PNG, decoded and brought down to at most HD_MAX_SCALE x the native nw x nh (any thread) */
static uint8_t *hd_load(uint64_t k, int nw, int nh, int *ow, int *oh)
{
    char path[1100]; snprintf(path, sizeof path, "%s/%016llx.png", tex_hd_dir(), (unsigned long long)k);
    int W = 0, H = 0; uint8_t *px = tex_hd_read_png(path, &W, &H);
    /* AT MOST 2x THE BOARD'S SIZE: the upscaler makes 4x, 16 times the texels -- the atlas overflowed and reset pages several times a
     * second, every texture on them baked again (seen as textures flashing). The 640x480 picture is drawn ~2.25x on a 1080p screen,
     * so 2x (a 2x2 average of the 4x image) loses next to nothing. */
    if (nw <= 0 && W % 4 == 0 && H % 4 == 0) { nw = W / 4; nh = H / 4; }   /* the native size unknown (the preload): the pack is the x4 model's */
    while (px && nw > 0 && nh > 0 && W % (2 * nw) == 0 && H % (2 * nh) == 0 && W / nw > HD_MAX_SCALE) {
        const int w2 = W / 2, h2 = H / 2;
        for (int y = 0; y < h2; y++) for (int x = 0; x < w2; x++) for (int c = 0; c < 4; c++) {
            const uint8_t *q = px + ((size_t)(2 * y) * W + 2 * x) * 4 + c;
            px[((size_t)y * w2 + x) * 4 + c] = (uint8_t)((q[0] + q[4] + q[(size_t)W * 4] + q[(size_t)W * 4 + 4] + 2) / 4);
        }
        W = w2; H = h2;
    }
    *ow = W; *oh = H;
    return px;
}
static void *hd_decoder(void *u)
{
    (void)u;
    pthread_mutex_lock(&hd_pmu);
    for (;;) {
        while (!hd_pqueued) pthread_cond_wait(&hd_pcv, &hd_pmu);
        int j = -1; for (int i = 0; i < HD_PEND; i++) if (hd_pend[i].st == 1) { j = i; break; }
        if (j < 0) { hd_pqueued = 0; continue; }
        hd_pend[j].st = 2; hd_pqueued--;
        const uint64_t k = hd_pend[j].k; const int nw = hd_pend[j].nw, nh = hd_pend[j].nh;
        pthread_mutex_unlock(&hd_pmu);
        int W = 0, H = 0; uint8_t *px = hd_load(k, nw, nh, &W, &H);
        pthread_mutex_lock(&hd_pmu);
        hd_pend[j].px = px; hd_pend[j].w = W; hd_pend[j].h = H; hd_pend[j].st = 3;   /* px NULL: not a usable PNG */
    }
    return NULL;
}
/* k's decoded image, or NULL: then its decode has been started (*pending = 1) unless the queue is full here */
static const uint8_t *hd_get(uint64_t k, int nw, int nh, int *w, int *h, int *pending)
{
    *pending = 0;
    const uint8_t *c = hd_cache_find(k, w, h); if (c) return c;
    pthread_mutex_lock(&hd_pmu);
    int j = hd_pfind(k);
    if (j >= 0 && hd_pend[j].st == 3) {
        uint8_t *px = hd_pend[j].px; const int W = hd_pend[j].w, H = hd_pend[j].h;
        hd_pend[j].px = NULL; hd_pend[j].st = 0;
        pthread_mutex_unlock(&hd_pmu);
        if (!px) return NULL;
        *w = W; *h = H; return hd_cache_put(k, px, W, H);
    }
    if (j < 0) {
        /* small images straight away, up to 2 ms of decoding a frame: most textures are a few dozen texels and decode in well
         * under a millisecond, and a texture drawn native for a frame, then sharp, is a visible flash; a burst (a new scene) goes
         * to the workers */
        extern unsigned g_eng_frame; static unsigned sync_frame; static double sync_ms;
        if (sync_frame != g_eng_frame) { sync_frame = g_eng_frame; sync_ms = 0; }
        if (sync_ms < 2.0 && !hd_preloading) {               /* (a big image's decode in the frame was a 0.4 s stall at the start) */
            pthread_mutex_unlock(&hd_pmu);
            struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
            int W = 0, H = 0; uint8_t *px = hd_load(k, nw, nh, &W, &H);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            sync_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) * 1e-6;
            if (!px) return NULL;
            *w = W; *h = H; return hd_cache_put(k, px, W, H);
        }
        const unsigned i0 = (unsigned)(k ^ (k >> 32)) % HD_PEND; int f = -1;
        for (int p = 0; p < HD_PEND_WIN && f < 0; p++) if (!hd_pend[(i0 + p) % HD_PEND].st) f = (int)((i0 + p) % HD_PEND);
        for (int p = 0; p < HD_PEND_WIN && f < 0; p++) {      /* a finished one its texture never came back for */
            const int q = (int)((i0 + p) % HD_PEND);
            if (hd_pend[q].st == 3) { free(hd_pend[q].px); hd_pend[q].px = NULL; hd_pend[q].st = 0; f = q; }
        }
        if (f < 0) { pthread_mutex_unlock(&hd_pmu); return NULL; }
        hd_pend[f].k = k; hd_pend[f].nw = nw; hd_pend[f].nh = nh; hd_pend[f].st = 1; hd_pqueued++;
        while (hd_pworkers < 3) { pthread_t t; if (pthread_create(&t, NULL, hd_decoder, NULL) != 0) break; pthread_detach(t); hd_pworkers++; }
        pthread_cond_signal(&hd_pcv);
    }
    pthread_mutex_unlock(&hd_pmu);
    *pending = 1;
    return NULL;
}
/* ---- THE WHOLE PACK IN MEMORY: at 2x it is ~400 MB (8,969 textures), so worker threads decode all of it in the background from the
 * first HD bake on (a few seconds); from then on every texture is sharp the first time it is drawn -- no native picture first (a
 * flash), no decode in the frame. Until a texture's turn comes, the on-demand path above serves it. Kept for the session. ---- */
#include <stdatomic.h>
static uint8_t **hd_pre; static int *hd_pre_w, *hd_pre_h; static _Atomic int *hd_pre_st;   /* per hd_idx[i]: 1 ready, 2 unusable */
static _Atomic int hd_pre_next, hd_pre_left; static struct timespec hd_pre_t0; static _Atomic size_t hd_pre_bytes;
static void *hd_preloader(void *u)
{
    (void)u;
    for (;;) {
        const int i = atomic_fetch_add(&hd_pre_next, 1);
        if (i >= (int)hd_n) break;
        int W = 0, H = 0; uint8_t *px = hd_load(hd_idx[i], 0, 0, &W, &H);
        hd_pre[i] = px; hd_pre_w[i] = W; hd_pre_h[i] = H;
        if (px) atomic_fetch_add(&hd_pre_bytes, (size_t)W * H * 4);
        atomic_store_explicit(&hd_pre_st[i], px ? 1 : 2, memory_order_release);
    }
    if (atomic_fetch_sub(&hd_pre_left, 1) == 1) {
        struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
        fprintf(stderr, "[TEX-HD] all %zu HD textures in memory (%.0f MB, %.1f s)\n", hd_n, atomic_load(&hd_pre_bytes) / 1048576.0,
                (t1.tv_sec - hd_pre_t0.tv_sec) + (t1.tv_nsec - hd_pre_t0.tv_nsec) * 1e-9);
    }
    return NULL;
}
static void hd_preload_start(void)
{
    static int done; if (done || !hd_n) return; done = 1;
    hd_preloading = 1;
    hd_pre = calloc(hd_n, sizeof *hd_pre); hd_pre_w = calloc(hd_n, sizeof *hd_pre_w); hd_pre_h = calloc(hd_n, sizeof *hd_pre_h);
    hd_pre_st = calloc(hd_n, sizeof *hd_pre_st);
    if (!hd_pre || !hd_pre_w || !hd_pre_h || !hd_pre_st) return;
    clock_gettime(CLOCK_MONOTONIC, &hd_pre_t0);
    enum { N = 4 };
    atomic_store(&hd_pre_left, N);
    pthread_attr_t a; pthread_attr_init(&a);
#ifdef __APPLE__
    pthread_attr_set_qos_class_np(&a, QOS_CLASS_UTILITY, 0);   /* below the game's own threads */
#endif
    for (int t = 0; t < N; t++) { pthread_t th; if (pthread_create(&th, &a, hd_preloader, NULL) == 0) pthread_detach(th); else atomic_fetch_sub(&hd_pre_left, 1); }
    pthread_attr_destroy(&a);
}

/* a bake that drew native while k decoded may be redone now (also when k's slot was given away: it is simply asked again) */
static int tex_hd_ready(uint64_t k)
{
    int w, h; if (hd_cache_find(k, &w, &h)) return 1;
    pthread_mutex_lock(&hd_pmu); const int j = hd_pfind(k); const int r = j < 0 || hd_pend[j].st == 3; pthread_mutex_unlock(&hd_pmu);
    return r;
}

/* the hook: after a native bake of w x h texels (stride bytes a row). Returns its HD replacement (owned by the cache, valid
 * until the next call), or NULL; collects the bake for upscaling when it has none. *wait: the HD image is still decoding
 * (this bake draws native until tex_hd_ready(*wait)). */
static const uint8_t *tex_hd_after_bake(const uint8_t *px, int stride, int w, int h, int *hw, int *hh, uint64_t *wait)
{
    *wait = 0;
    if (!tex_hd_enabled()) return NULL;
    hd_index();
    hd_preload_start();
    const uint64_t k = tex_hd_hash(px, stride, w, h);
    const uint64_t *found = hd_n ? bsearch(&k, hd_idx, hd_n, sizeof k, hd_cmp) : NULL;
    if (found) {
        const size_t i = (size_t)(found - hd_idx);
        int W = 0, H = 0, pending = 0; const uint8_t *img;
        if (hd_pre_st && atomic_load_explicit(&hd_pre_st[i], memory_order_acquire) == 1) { img = hd_pre[i]; W = hd_pre_w[i]; H = hd_pre_h[i]; }
        else img = hd_get(k, w, h, &W, &H, &pending);
        if (pending) { *wait = k; g_tex_hd_waits++; return NULL; }
        if (img && !(W % w) && !(H % h) && W / w == H / h && W / w >= 2 && W <= 2048 && H <= 2048) {
            { static unsigned n; if (++n == 1 || n % 1000 == 0) fprintf(stderr, "[TEX-HD] %u HD textures drawn (latest %dx%d -> %dx%d)\n", n, w, h, W, H); }
            *hw = W; *hh = H; return img;
        }
        return NULL;
    }
    const char *dump = tex_hd_dump_dir();
    if (dump && w * h >= 64) {                                /* tiny ones (under 8x8) are not worth an upscale */
        static uint64_t *seen; static size_t ns, cs;          /* this session's dumps: no stat for a texture already written */
        for (size_t i = 0; i < ns; i++) if (seen[i] == k) return NULL;
        if (ns == cs) { cs = cs ? cs * 2 : 1024; uint64_t *n2 = realloc(seen, cs * sizeof *n2); if (!n2) return NULL; seen = n2; }
        seen[ns++] = k;
        char path[1100]; snprintf(path, sizeof path, "%s/%016llx.png", dump, (unsigned long long)k);
        struct stat st; if (stat(path, &st) != 0) { hd_queue_write(path, px, stride, w, h); g_tex_hd_dumped++; }
    }
    return NULL;
}

/* at quit: what this session collected goes to the upscaler, detached (its own session: it outlives the game) */
#include <spawn.h>
extern char **environ;
void tex_hd_quit(void)
{
    hd_queue_drain();
    const char *cmd = getenv("ENG_TEX_UPSCALE_CMD");
    if (!g_tex_hd_dumped || !cmd || !tex_hd_dir() || !tex_hd_dump_dir()) return;
    posix_spawnattr_t a; posix_spawnattr_init(&a); posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
    char *argv[] = { "/bin/sh", (char *)cmd, (char *)tex_hd_dir(), (char *)tex_hd_dump_dir(), NULL };
    pid_t pid = 0;
    const int r = posix_spawn(&pid, "/bin/sh", NULL, &a, argv, environ);
    posix_spawnattr_destroy(&a);
    fprintf(stderr, "[TEX-HD] %u new textures collected: %s\n", g_tex_hd_dumped, r == 0 ? "upscaling them in the background" : "the upscaler did not start");
}
#endif
