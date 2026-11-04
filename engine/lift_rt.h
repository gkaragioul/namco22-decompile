/*
 * lift_rt.h -- runtime for the LIFTED 68K programs of every System 22 game
 * (tools/namco22/lift.py). Shared: was raverace/include/rr_lift_rt.h.
 *
 * The generated code's ABI is the rr_* one: R[], rr_read/rr_write (the game's memory
 * map, declared here), the shadow return stack and interrupt entry (engine/lift_cpu.c).
 * Rave Racer's rr_lift_rt.h is now this file plus rr_mem.h.
 *
 * R[] is Ghidra's 68020 register space as big-endian bytes: D0 at 0x00,
 * A0 at 0x20, SP (A7) at 0x3C, the flag bytes at 0x40.., PC 0x50, SR 0x200,
 * ISP/MSP/VBR/CACR at 0x100.. -- every varnode is a BE load/store at its
 * offset, which makes sub-register writes (D0w, D0b) exact.
 */
#ifndef LIFT_RT_H
#define LIFT_RT_H
#include <stdint.h>

/* the game's memory map: every 68K access in the generated code lands here, big-endian, at the
 * original address and the width the instruction used (size 1, 2 or 4) */
uint32_t rr_read(uint32_t a, int size);
void     rr_write(uint32_t a, int size, uint32_t v);

#define RR_REGSPACE 0x800
extern uint8_t R[RR_REGSPACE];
extern int32_t rr_budget;

static inline uint64_t RG1(uint32_t o) { return R[o]; }
static inline uint64_t RG2(uint32_t o) { return (uint64_t)R[o] << 8 | R[o + 1]; }
static inline uint64_t RG4(uint32_t o) { return (uint64_t)R[o] << 24 | (uint64_t)R[o + 1] << 16 | (uint64_t)R[o + 2] << 8 | R[o + 3]; }
static inline uint64_t RG8(uint32_t o) { return RG4(o) << 32 | RG4(o + 4); }
static inline void RS1(uint32_t o, uint64_t v) { R[o] = (uint8_t)v; }
static inline void RS2(uint32_t o, uint64_t v) { R[o] = (uint8_t)(v >> 8); R[o + 1] = (uint8_t)v; }
static inline void RS4(uint32_t o, uint64_t v) { R[o] = (uint8_t)(v >> 24); R[o + 1] = (uint8_t)(v >> 16); R[o + 2] = (uint8_t)(v >> 8); R[o + 3] = (uint8_t)v; }
static inline void RS8(uint32_t o, uint64_t v) { RS4(o, v >> 32); RS4(o + 4, v); }
/* 80-bit extended-precision registers: 68881 opcodes Ghidra reads out of DATA regions (a 68EC020 has no FPU and the traces never execute them);
 * kept as their low 64 bits so the file compiles, not as floating point. */
static inline uint64_t RG10(uint32_t o) { return RG8(o + 2); }
static inline void RS10(uint32_t o, uint64_t v) { R[o] = 0; R[o + 1] = 0; RS8(o + 2, v); }

static inline uint32_t MRD1(uint32_t a) { return rr_read(a, 1); }
static inline uint32_t MRD2(uint32_t a) { return rr_read(a, 2); }
static inline uint32_t MRD4(uint32_t a) { return rr_read(a, 4); }
static inline uint64_t MRD8(uint32_t a) { return (uint64_t)rr_read(a, 4) << 32 | rr_read(a + 4, 4); }
static inline void MWR1(uint32_t a, uint64_t v) { rr_write(a, 1, (uint32_t)v); }
static inline void MWR2(uint32_t a, uint64_t v) { rr_write(a, 2, (uint32_t)v); }
static inline void MWR4(uint32_t a, uint64_t v) { rr_write(a, 4, (uint32_t)v); }
static inline void MWR8(uint32_t a, uint64_t v) { rr_write(a, 4, (uint32_t)(v >> 32)); rr_write(a + 4, 4, (uint32_t)v); }

static inline int64_t SX1(uint64_t v) { return (int8_t)v; }
static inline int64_t SX2(uint64_t v) { return (int16_t)v; }
static inline int64_t SX4(uint64_t v) { return (int32_t)v; }
static inline int64_t SX8(uint64_t v) { return (int64_t)v; }
static inline int64_t SXN(uint64_t v, int sz) {
    return sz == 1 ? SX1(v) : sz == 2 ? SX2(v) : sz == 4 ? SX4(v) : (int64_t)v;
}
static inline uint64_t MASKN(uint64_t v, int sz) { return sz >= 8 ? v : v & ((1ULL << (8 * sz)) - 1); }

/* p-code shifts: an amount >= the operand width gives 0 (or the sign) */
static inline uint64_t SHL(uint64_t a, uint64_t n, int sz) { return n >= (uint64_t)(8 * sz) ? 0 : MASKN(a << n, sz); }
static inline uint64_t SHR(uint64_t a, uint64_t n, int sz) { return n >= (uint64_t)(8 * sz) ? 0 : MASKN(a, sz) >> n; }
static inline uint64_t SAR(uint64_t a, uint64_t n, int sz) {
    int64_t s = SXN(a, sz);
    return (uint64_t)(n >= (uint64_t)(8 * sz) ? (s < 0 ? -1 : 0) : (s >> n));
}
static inline uint64_t CARRY(uint64_t a, uint64_t b, int sz) {
    return (MASKN(a, sz) + MASKN(b, sz)) >> (8 * sz) & 1 ? 1 : (sz == 8 && MASKN(a, 8) + MASKN(b, 8) < MASKN(a, 8));
}
static inline uint64_t SCARRY(uint64_t a, uint64_t b, int sz) {
    int64_t x = SXN(a, sz), y = SXN(b, sz), r = SXN((uint64_t)(x + y), sz);
    return (x >= 0) == (y >= 0) && (r >= 0) != (x >= 0);
}
static inline uint64_t SBORROW(uint64_t a, uint64_t b, int sz) {
    int64_t x = SXN(a, sz), y = SXN(b, sz), r = SXN((uint64_t)(x - y), sz);
    return (x >= 0) != (y >= 0) && (r >= 0) != (x >= 0);
}

void rr_trap(uint32_t at, uint32_t target, const char *what);
void rr_div0(const char *what);   /* the 68K takes vector 5 (a game restarts there, or the ROM has a bare rte): counted and reported, execution goes on with 0 */
static inline uint64_t UDIVREM(uint64_t a, uint64_t b, char op) {
    if (!b) { rr_div0("unsigned divide by zero"); return 0; }
    return op == '/' ? a / b : a % b;
}
static inline int64_t SDIVREM(int64_t a, int64_t b, char op) {
    if (!b) { rr_div0("signed divide by zero"); return 0; }
    if (b == -1) return op == '/' ? -a : 0;
    return op == '/' ? a / b : a % b;
}

/* 68020 decimal arithmetic -- Ghidra's bcdAdjust leaves the byte binary, so
 * abcd/sbcd/nbcd are done here. X = C = decimal carry/borrow; Z is cleared
 * if the result is non-zero and otherwise UNCHANGED (multi-byte chains). */
#define RR_XF 0x43
#define RR_CF 0x47
#define RR_ZF 0x45
static inline uint32_t rr_abcd(uint32_t dst, uint32_t src) {
    uint32_t x = R[RR_XF] & 1;
    uint32_t lo = (dst & 0xF) + (src & 0xF) + x;
    uint32_t r = (dst & 0xF0) + (src & 0xF0);
    if (lo > 9) lo += 6;
    r += lo;
    uint32_t c = 0;
    if (r > 0x99) { r += 0x60; c = 1; }
    r &= 0xFF;
    R[RR_XF] = R[RR_CF] = (uint8_t)c;
    if (r) R[RR_ZF] = 0;
    return r;
}
static inline uint32_t rr_sbcd(uint32_t dst, uint32_t src) {
    uint32_t x = R[RR_XF] & 1;
    int lo = (int)(dst & 0xF) - (int)(src & 0xF) - (int)x;
    int hi = (int)(dst & 0xF0) - (int)(src & 0xF0);
    uint32_t c = 0;
    if (lo < 0) { lo -= 6; hi -= 0x10; }
    int r = hi + (lo & 0xF);
    if (hi < 0) { r -= 0x60; c = 1; }
    r &= 0xFF;
    R[RR_XF] = R[RR_CF] = (uint8_t)c;
    if (r) R[RR_ZF] = 0;
    return (uint32_t)r;
}

/* ---- 68020 bit-fields: bfextu / bfexts / bfffo / bftst / bfclr / bfset / bfchg / bfins (tools/namco22/lift.py emit_bitfield). Done here, by the manual's
 * rules, because Ghidra's p-code gets bfclr / bfset / bfchg on MEMORY wrong: it reuses one temporary for the whole word and for the extracted field, so the
 * instruction stores the FIELD (0..3) over the entire 32-bit word (Dirt Dash: the car's damage bits 0x55555000 became 0x00000001 at the first crash, and
 * the truck's body model was looked up at index -1).
 * A field is `w` bits (1..32) starting `off` bits below the MSB. In a REGISTER the offset is taken mod 32 and the field wraps around inside the register.
 * In MEMORY the offset is signed and counts from the MSB of the byte at ea: the field lives in the (bit + w + 7) / 8 bytes from ea + (off >> 3), and only
 * those bytes are read and written back. Flags: N = the field's top bit, Z = field == 0, V = C = 0 (X untouched); bfins takes them from the value inserted. */
static inline uint32_t rr_bf_mask(uint32_t w) { return w >= 32 ? 0xFFFFFFFFu : ((1u << w) - 1); }
static inline uint32_t rr_bf_reg_get(uint32_t v, int32_t off, uint32_t w)
{
    uint32_t o = (uint32_t)off & 31, rot = o ? (v << o) | (v >> (32 - o)) : v;
    return rot >> (32 - w);
}
static inline uint32_t rr_bf_reg_set(uint32_t v, int32_t off, uint32_t w, uint32_t fld)
{
    uint32_t o = (uint32_t)off & 31, rot = o ? (v << o) | (v >> (32 - o)) : v;
    uint32_t m = rr_bf_mask(w) << (32 - w);
    rot = (rot & ~m) | ((fld << (32 - w)) & m);
    return o ? (rot >> o) | (rot << (32 - o)) : rot;
}
static inline uint32_t rr_bf_mem_get(uint32_t ea, int32_t off, uint32_t w)
{
    uint32_t a = ea + (uint32_t)(off >> 3), bit = (uint32_t)off & 7, n = (bit + w + 7) >> 3;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = v << 8 | MRD1(a + i);
    return (uint32_t)((v >> (n * 8 - bit - w)) & rr_bf_mask(w));
}
static inline void rr_bf_mem_set(uint32_t ea, int32_t off, uint32_t w, uint32_t fld)
{
    uint32_t a = ea + (uint32_t)(off >> 3), bit = (uint32_t)off & 7, n = (bit + w + 7) >> 3, sh = n * 8 - bit - w;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) v = v << 8 | MRD1(a + i);
    uint64_t m = (uint64_t)rr_bf_mask(w) << sh;
    v = (v & ~m) | (((uint64_t)fld << sh) & m);
    for (uint32_t i = 0; i < n; i++) MWR1(a + i, (v >> ((n - 1 - i) * 8)) & 0xFF);
}
static inline void rr_bf_flags(uint32_t fld, uint32_t w)
{
    R[0x44] = (uint8_t)((fld >> (w - 1)) & 1);      /* N */
    R[0x45] = fld == 0;                              /* Z */
    R[0x46] = 0; R[0x47] = 0;                        /* V, C */
}

/* SR lives in Ghidra's flag bytes: 0x40 T, 0x41 S, 0x42 IPL (0..7), 0x43 X,
 * 0x44 N, 0x45 Z, 0x46 V, 0x47 C. Ghidra's move-from-SR p-code composes it
 * in ONE-BYTE temporaries, so IPL<<8, S<<13 and T<<15 shift out to zero and
 * only the CCR survives -- the lifter substitutes rr_get_sr() for that. */
static inline uint32_t rr_get_sr(void)
{
    return (R[0x40] & 1u) << 15 | (R[0x41] & 1u) << 13 | (R[0x42] & 7u) << 8 | (R[0x43] & 1u) << 4 |
           (R[0x44] & 1u) << 3 | (R[0x45] & 1u) << 2 | (R[0x46] & 1u) << 1 | (R[0x47] & 1u);
}
static inline void rr_set_sr(uint32_t sr)
{
    R[0x40] = sr >> 15 & 1; R[0x41] = sr >> 13 & 1; R[0x42] = sr >> 8 & 7; R[0x43] = sr >> 4 & 1;
    R[0x44] = sr >> 3 & 1; R[0x45] = sr >> 2 & 1; R[0x46] = sr >> 1 & 1; R[0x47] = sr & 1;
    RS2(0x200, sr);
}

/* The scheduler hook: every call and backward branch counts down a budget;
 * when it runs out the host delivers interrupts and steps the other chips. */
void rr_tick(void);
/* rd_poll_rec (src/rd/rd_core.c): during a check, the register file is recorded at
 * every poll -- an interrupt can only land there, and the vblank handler saves the
 * registers to WRAM, so a readable function must hold the 68K's registers at each one */
extern int rd_poll_rec;
void rd_poll_snap(void);
#define RR_POLL() do { if (rd_poll_rec) rd_poll_snap(); if (rr_budget <= 0) rr_tick(); } while (0)

void rr_call_ind(uint32_t target, uint32_t at);

/* Shadow return stack. The lifted code turns every 68K call into a C call,
 * but 68K code also uses rts as a computed jump (pea label; ...; rts) and
 * returns past several frames. Each call pushes (expected return, SP); rts
 * with a matching entry unwinds the C frames to that call site
 * (rr_after_call), anything else is a jump. rr_ret_to = where rts went. */
extern uint32_t rr_ret_to;
int  rr_call_push(uint32_t ret);      /* -> this call's shadow index */
int  rr_after_call(int j);            /* 1 = keep unwinding past this frame */
int  rr_return(uint32_t target);      /* 1 = matched a call site: C return */
void rr_rte(void);
int  rr_irq_push(void);

/* Per-instruction hook. Empty in the normal build; the trace build
 * (-DRR_TRACE) logs every instruction with the registers, in a format
 * tools/diff_trace_mame.py lines up against MAME's debugger trace. */
/* Time is counted in 68K instructions: every instruction spends one unit of
 * rr_budget; RR_POLL (calls, backward branches) hands over to the scheduler
 * once it runs out. */
#ifdef RR_TRACE
void rr_trace_ins(uint32_t pc);
#define RR_INS(a) (rr_trace_ins(a), --rr_budget)
#elif defined(GLMC_METAL)
/* the Mac build's save states (engine/ss22_state.c): the instruction a poll follows -- a loop's branch is where a save can resume */
extern uint32_t rr_pc_now;
#define RR_INS(a) (rr_pc_now = (a), --rr_budget)
#else
#define RR_INS(a) (--rr_budget)
#endif

#endif
