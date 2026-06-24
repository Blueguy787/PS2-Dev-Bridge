/**
 * regdump.c — Phase 2a
 *
 * Dumps all EE processor registers to the PC via the stdout ring:
 *   - All 32 GPRs (128-bit, printed as hi64:lo64)
 *   - HI, LO (multiply/divide accumulators)
 *   - COP0 control registers (Status, Cause, EPC, BadVAddr, etc.)
 *   - COP1 FPU registers (32 x f32) + FCR31 (control/status)
 *   - SA (shift amount register, EE-specific)
 *
 * These are the registers at the point main() is entered — not a
 * mid-execution snapshot. For live game register inspection you want
 * the GDB stub (Phase 4). This proves the introspection path works
 * and gives you the EE state at stub handoff.
 *
 * Push with:
 *   python sd2psx_cli.py elf regdump.elf
 *
 * Expected output (excerpt):
 *   [INF] ======== EE Register Dump ========
 *   [REG] GPR  r0/zero  0000000000000000:0000000000000000
 *   [REG] GPR  r1/at    0000000000000000:00000000001000A4
 *   ...
 *   [REG] COP0 Status   0x70400C13
 *   [REG] COP0 Cause    0x00000000
 *   ...
 *   [REG] FPU  f0       0.000000  (0x00000000)
 *   ...
 *   [INF] ======== done ========
 */

#include <tamtypes.h>
#include <kernel.h>
#include <string.h>

#include "../phase1/dev_io.h"

/* ── Register capture structures ─────────────────────────────────────────── */

/* 128-bit GPR — EE GPRs are 128 bits wide */
typedef struct {
    u64 lo;
    u64 hi;
} Reg128;

typedef struct {
    Reg128 gpr[32];
    u64    hi_acc;
    u64    lo_acc;
    u32    sa;
    /* COP0 */
    u32    cop0_index;
    u32    cop0_random;
    u32    cop0_entrylo0;
    u32    cop0_entrylo1;
    u32    cop0_context;
    u32    cop0_pagemask;
    u32    cop0_wired;
    u32    cop0_badvaddr;
    u32    cop0_count;
    u32    cop0_entryhi;
    u32    cop0_compare;
    u32    cop0_status;
    u32    cop0_cause;
    u32    cop0_epc;
    u32    cop0_prid;
    u32    cop0_config;
    u32    cop0_badpaddr;
    u32    cop0_debug;
    u32    cop0_perf;
    u32    cop0_taglo;
    u32    cop0_taghi;
    u32    cop0_errorepc;
    /* COP1 FPU */
    float  fpr[32];
    u32    fcr31;
} EERegs;

static EERegs _regs __attribute__((aligned(16)));

/* ── GPR names ───────────────────────────────────────────────────────────── */

static const char *gpr_name[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

/* ── Capture routines ────────────────────────────────────────────────────── */

static void capture_gprs(EERegs *r) {
    /* Capture all 32 GPRs using inline asm sq (store quadword).
     * sq stores all 128 bits of a GPR. We use a temp array aligned to 16. */
    asm volatile (
        "sq  $0,   0*16(%0)\n"   "sq  $1,   1*16(%0)\n"
        "sq  $2,   2*16(%0)\n"   "sq  $3,   3*16(%0)\n"
        "sq  $4,   4*16(%0)\n"   "sq  $5,   5*16(%0)\n"
        "sq  $6,   6*16(%0)\n"   "sq  $7,   7*16(%0)\n"
        "sq  $8,   8*16(%0)\n"   "sq  $9,   9*16(%0)\n"
        "sq  $10, 10*16(%0)\n"   "sq  $11, 11*16(%0)\n"
        "sq  $12, 12*16(%0)\n"   "sq  $13, 13*16(%0)\n"
        "sq  $14, 14*16(%0)\n"   "sq  $15, 15*16(%0)\n"
        "sq  $16, 16*16(%0)\n"   "sq  $17, 17*16(%0)\n"
        "sq  $18, 18*16(%0)\n"   "sq  $19, 19*16(%0)\n"
        "sq  $20, 20*16(%0)\n"   "sq  $21, 21*16(%0)\n"
        "sq  $22, 22*16(%0)\n"   "sq  $23, 23*16(%0)\n"
        "sq  $24, 24*16(%0)\n"   "sq  $25, 25*16(%0)\n"
        "sq  $26, 26*16(%0)\n"   "sq  $27, 27*16(%0)\n"
        "sq  $28, 28*16(%0)\n"   "sq  $29, 29*16(%0)\n"
        "sq  $30, 30*16(%0)\n"   "sq  $31, 31*16(%0)\n"
        : : "r"(r->gpr) : "memory"
    );
}

static void capture_hi_lo_sa(EERegs *r) {
    asm volatile (
        "mfhi %0\n"
        "mflo %1\n"
        "mfsa %2\n"
        : "=r"(r->hi_acc), "=r"(r->lo_acc), "=r"(r->sa)
    );
}

static void capture_cop0(EERegs *r) {
#define MFC0(dst, reg, sel) \
    asm volatile ("mfc0 %0, $" #reg ", " #sel : "=r"(dst))

    MFC0(r->cop0_index,    0, 0);
    MFC0(r->cop0_random,   1, 0);
    MFC0(r->cop0_entrylo0, 2, 0);
    MFC0(r->cop0_entrylo1, 3, 0);
    MFC0(r->cop0_context,  4, 0);
    MFC0(r->cop0_pagemask, 5, 0);
    MFC0(r->cop0_wired,    6, 0);
    MFC0(r->cop0_badvaddr, 8, 0);
    MFC0(r->cop0_count,    9, 0);
    MFC0(r->cop0_entryhi, 10, 0);
    MFC0(r->cop0_compare, 11, 0);
    MFC0(r->cop0_status,  12, 0);
    MFC0(r->cop0_cause,   13, 0);
    MFC0(r->cop0_epc,     14, 0);
    MFC0(r->cop0_prid,    15, 0);
    MFC0(r->cop0_config,  16, 0);
    MFC0(r->cop0_badpaddr,23, 0);
    MFC0(r->cop0_debug,   24, 0);
    MFC0(r->cop0_perf,    25, 0);
    MFC0(r->cop0_taglo,   28, 0);
    MFC0(r->cop0_taghi,   29, 0);
    MFC0(r->cop0_errorepc,30, 0);
#undef MFC0
}

static void capture_fpu(EERegs *r) {
#define MFC1(idx) \
    asm volatile ("mfc1 %0, $f" #idx : "=r"(*(u32*)&r->fpr[idx]))

    MFC1(0);  MFC1(1);  MFC1(2);  MFC1(3);
    MFC1(4);  MFC1(5);  MFC1(6);  MFC1(7);
    MFC1(8);  MFC1(9);  MFC1(10); MFC1(11);
    MFC1(12); MFC1(13); MFC1(14); MFC1(15);
    MFC1(16); MFC1(17); MFC1(18); MFC1(19);
    MFC1(20); MFC1(21); MFC1(22); MFC1(23);
    MFC1(24); MFC1(25); MFC1(26); MFC1(27);
    MFC1(28); MFC1(29); MFC1(30); MFC1(31);
#undef MFC1
    asm volatile ("cfc1 %0, $31" : "=r"(r->fcr31));
}

/* ── Print ───────────────────────────────────────────────────────────────── */

static void print_regs(const EERegs *r) {
    DEV_INF("======== EE Register Dump ========");

    /* GPRs */
    DEV_INF("-- GPRs (128-bit hi:lo) --");
    for (int i = 0; i < 32; i++) {
        DEV_REG("r%02d/%-4s  %016llX:%016llX",
                i, gpr_name[i],
                (unsigned long long)r->gpr[i].hi,
                (unsigned long long)r->gpr[i].lo);
    }

    /* HI / LO / SA */
    DEV_INF("-- Accumulators --");
    DEV_REG("HI       %016llX", (unsigned long long)r->hi_acc);
    DEV_REG("LO       %016llX", (unsigned long long)r->lo_acc);
    DEV_REG("SA       0x%08lX", (unsigned long)r->sa);

    /* COP0 */
    DEV_INF("-- COP0 --");
    DEV_REG("Status   0x%08lX", (unsigned long)r->cop0_status);
    DEV_REG("Cause    0x%08lX", (unsigned long)r->cop0_cause);
    DEV_REG("EPC      0x%08lX", (unsigned long)r->cop0_epc);
    DEV_REG("BadVAddr 0x%08lX", (unsigned long)r->cop0_badvaddr);
    DEV_REG("Count    0x%08lX", (unsigned long)r->cop0_count);
    DEV_REG("Compare  0x%08lX", (unsigned long)r->cop0_compare);
    DEV_REG("PRID     0x%08lX", (unsigned long)r->cop0_prid);
    DEV_REG("Config   0x%08lX", (unsigned long)r->cop0_config);
    DEV_REG("EntryHi  0x%08lX", (unsigned long)r->cop0_entryhi);
    DEV_REG("Context  0x%08lX", (unsigned long)r->cop0_context);
    DEV_REG("Wired    0x%08lX", (unsigned long)r->cop0_wired);
    DEV_REG("ErrorEPC 0x%08lX", (unsigned long)r->cop0_errorepc);

    /* COP1 FPU */
    DEV_INF("-- COP1 FPU --");
    for (int i = 0; i < 32; i++) {
        u32 raw;
        memcpy(&raw, &r->fpr[i], 4);
        DEV_REG("f%02d      %12.6f  (0x%08lX)", i,
                (double)r->fpr[i], (unsigned long)raw);
    }
    DEV_REG("FCR31    0x%08lX", (unsigned long)r->fcr31);

    DEV_INF("======== done ========");
}

/* ── Entry ───────────────────────────────────────────────────────────────── */

int main(void) {
    dev_io_init();

    /* Capture in dependency order — GPRs first before anything else
     * clobbers them, then accumulators, then COP0/COP1 */
    capture_gprs(&_regs);
    capture_hi_lo_sa(&_regs);
    capture_cop0(&_regs);
    capture_fpu(&_regs);

    print_regs(&_regs);
    return 0;
}
