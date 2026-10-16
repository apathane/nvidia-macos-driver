#pragma once
#include <stdint.h>
#include <stddef.h>

// CACHEPROBE-r1: pure decoding of the x86-64 memory type of one kernel virtual
// address. The caller supplies CR3, PAT, MTRR values and a physical reader, so
// this file holds no privileged instruction and is testable in user space.
// Reads only; never maps, writes or follows the probed address itself.

enum { NVRM_MT_UC = 0, NVRM_MT_WC = 1, NVRM_MT_WT = 4, NVRM_MT_WP = 5, NVRM_MT_WB = 6,
       NVRM_MT_UCMINUS = 7, NVRM_MT_MIXED = 0xfe, NVRM_MT_UNKNOWN = 0xff };

static inline const char *nvrmMemTypeName(unsigned t)
{
    switch (t) {
    case NVRM_MT_UC: return "UC"; case NVRM_MT_WC: return "WC"; case NVRM_MT_WT: return "WT";
    case NVRM_MT_WP: return "WP"; case NVRM_MT_WB: return "WB"; case NVRM_MT_UCMINUS: return "UC-";
    case NVRM_MT_MIXED: return "MIXED"; default: return "?";
    }
}

typedef int (*nvrmProbePhysRead64)(uint64_t pa, uint64_t *value, void *context);

struct NVRMCacheWalk {
    int ok;            // 1 = present leaf found
    int level;         // 1 = 4 KiB, 2 = 2 MiB, 3 = 1 GiB
    uint64_t entry;    // leaf entry
    uint64_t pa;       // translated physical address
    unsigned patIndex; // 0..7
    int failStep;      // table level where the walk stopped (4..1), 0 if ok
};

static const uint64_t kNvrmPteAddr = 0x000ffffffffff000ull;

static inline NVRMCacheWalk nvrmCacheWalk(uint64_t cr3, uint64_t va, nvrmProbePhysRead64 rd, void *ctx)
{
    NVRMCacheWalk w = {};
    uint64_t table = cr3 & kNvrmPteAddr;
    for (int lvl = 4; lvl >= 1; --lvl) {
        const unsigned shift = 12 + 9 * (unsigned)(lvl - 1);
        const uint64_t idx = (va >> shift) & 511;
        uint64_t e = 0;
        if (rd(table + idx * 8, &e, ctx) || !(e & 1)) { w.failStep = lvl; return w; }
        const bool leaf = (lvl == 1) || ((lvl == 2 || lvl == 3) && (e & 0x80));
        if (!leaf) { table = e & kNvrmPteAddr; continue; }
        const uint64_t pageMask = (1ull << shift) - 1;
        // Large pages keep PAT in bit 12, so drop bits 12..shift-1 from the frame.
        w.pa = (e & kNvrmPteAddr & ~pageMask) | (va & pageMask);
        const unsigned patBit = (lvl == 1) ? (unsigned)((e >> 7) & 1) : (unsigned)((e >> 12) & 1);
        w.patIndex = (patBit << 2) | (unsigned)(((e >> 4) & 1) << 1) | (unsigned)((e >> 3) & 1);
        w.entry = e; w.level = lvl; w.ok = 1;
        return w;
    }
    return w;
}

static inline unsigned nvrmPatType(uint64_t patMsr, unsigned index)
{
    return (unsigned)((patMsr >> (8 * (index & 7))) & 7);
}

struct NVRMMtrrState {
    uint64_t defType;          // IA32_MTRR_DEF_TYPE
    unsigned count;            // variable ranges used (<= 16)
    uint64_t base[16], mask[16];
    int amdTom2;               // 1 = SYSCFG.MtrrTom2En (bit 21) and Tom2ForceMemTypeWB (bit 22) both set
    uint64_t syscfg;           // raw, for the report
    uint64_t tom2;
};

// AMD SYSCFG (MSR C001_0010): bit 21 MtrrTom2En, bit 22 Tom2ForceMemTypeWB. Both are needed for [4 GiB, TOM2) to be WB.
static inline int nvrmAmdTom2ForcesWb(uint64_t syscfg)
{
    return (syscfg & (1ull << 21)) && (syscfg & (1ull << 22));
}

// Memory type the MTRRs give to one physical address above 1 MiB. Every variable range is examined first, so the
// answer does not depend on range order: any UC wins, WT+WB gives WT, one type gives itself, else undefined.
static inline unsigned nvrmMtrrType(const NVRMMtrrState *m, uint64_t pa)
{
    if (!(m->defType & (1ull << 11))) return NVRM_MT_UC; // MTRRs disabled
    unsigned seen = 0; // bit per memory type 0..7
    for (unsigned i = 0; i < m->count && i < 16; ++i) {
        if (!(m->mask[i] & (1ull << 11))) continue;
        const uint64_t mk = m->mask[i] & kNvrmPteAddr;
        if ((pa & mk) != (m->base[i] & mk)) continue;
        const unsigned t = (unsigned)(m->base[i] & 0xff);
        if (t > 6 || t == 2 || t == 3) return NVRM_MT_UNKNOWN; // reserved encodings (7 is PAT-only UC-)
        seen |= 1u << t;
    }
    if (seen) {
        if (seen & (1u << NVRM_MT_UC)) return NVRM_MT_UC;
        if (seen == ((1u << NVRM_MT_WT) | (1u << NVRM_MT_WB))) return NVRM_MT_WT;
        if (!(seen & (seen - 1))) { unsigned t = 0; while (!(seen & (1u << t))) ++t; return t; }
        return NVRM_MT_MIXED;
    }
    // AMD: with SYSCFG.MtrrTom2En AND SYSCFG.Tom2ForceMemTypeWB, unmatched memory in [4 GiB, TOM2) is WB instead of
    // the default type. amdTom2 must only be set when both bits are; MtrrTom2En alone does not change the type.
    if (m->amdTom2 && pa >= (1ull << 32) && pa < (m->tom2 & kNvrmPteAddr)) return NVRM_MT_WB;
    return (unsigned)(m->defType & 0xff);
}

// PAT x MTRR combination (Intel SDM 12.5.2.2 / AMD APM 7.8.2), the cases a
// framebuffer or a shared surface can reach. Anything else is reported raw.
static inline unsigned nvrmEffectiveType(unsigned pat, unsigned mtrr)
{
    if (mtrr == NVRM_MT_MIXED || mtrr == NVRM_MT_UNKNOWN) return NVRM_MT_UNKNOWN;
    switch (pat) {
    case NVRM_MT_UC: return NVRM_MT_UC;
    case NVRM_MT_WC: return NVRM_MT_WC;
    case NVRM_MT_UCMINUS: return mtrr == NVRM_MT_WC ? NVRM_MT_WC : NVRM_MT_UC;
    case NVRM_MT_WB: return mtrr;
    case NVRM_MT_WT:
        if (mtrr == NVRM_MT_WB || mtrr == NVRM_MT_WT) return NVRM_MT_WT;
        return mtrr == NVRM_MT_WP ? NVRM_MT_WP : NVRM_MT_UC;
    case NVRM_MT_WP:
        if (mtrr == NVRM_MT_WB || mtrr == NVRM_MT_WP) return NVRM_MT_WP;
        return NVRM_MT_UC;
    default: return NVRM_MT_UNKNOWN;
    }
}

// SCANOUTWC-r1 policy for one page of the WC alias. MTRR must be UC: then every PAT type any other mapping of the
// same page can use resolves to UC (UC, UC-, WT, WP, WB) or WC, never a cacheable type, so the alias cannot meet a
// cached copy of the page (the WC-vs-cacheable aliasing the SDM/APM forbid). And this mapping itself must be WC.
static inline int nvrmScanoutWcPageOk(unsigned patType, unsigned mtrrType)
{
    if (mtrrType != NVRM_MT_UC) return 0;
    static const unsigned kPatTypes[] = {NVRM_MT_UC, NVRM_MT_WC, NVRM_MT_WT, NVRM_MT_WP, NVRM_MT_WB, NVRM_MT_UCMINUS};
    for (unsigned i = 0; i < sizeof kPatTypes / sizeof kPatTypes[0]; ++i) {
        const unsigned e = nvrmEffectiveType(kPatTypes[i], mtrrType);
        if (e != NVRM_MT_UC && e != NVRM_MT_WC) return 0; // defensive: proves the claim above for this MTRR type
    }
    return nvrmEffectiveType(patType, mtrrType) == NVRM_MT_WC;
}
