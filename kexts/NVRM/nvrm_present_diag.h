#pragma once
// Read-only local diagnostic ABI. Does not change the allocation or flip ABI.
#define NVRM_PRESENT_TIMING_FN "nvPresentTimingSnapshot"
#define NVRM_PRESENT_DIAG_VERSION 1ull
#define NVRM_PRESENT_HEADS 5u
#define NVRM_PRESENT_BUCKETS 16u
#define NVRM_PRESENT_EVENTS 64u
struct NVRMPresentTiming {
    unsigned long long version, lastVblankNs, periodNs, vblankSequence;
    unsigned long long hardwareLive, modeEpoch, lastTimerVblankNs, timerVblankSequence;
};
struct NVRMPresentEvent {
    unsigned long long beginNs, readyNs, copyNs, totalNs, phaseNs, periodNs;
    unsigned long long vblankBefore, vblankAfter, bytes, surfaceId, head, outcome, flags;
};
// outcomes: 0 failed, 1 copied, 2 direct flip, 3 held, 4 empty.
// flags: bit 0 live hardware timing sampled, bit 1 crossed observed vblank,
// bit 2 modeset during copy, bit 3 timing unavailable/stale,
// bit 4 fallback timer sampled (NOT panel vblank), bit 5 crossed fallback timer.
struct NVRMPresentHead {
    unsigned long long attempts, copied, flips, held, failed, empty;
    unsigned long long totalNs, maxTotalNs, readyNs, maxReadyNs, copyNs, maxCopyNs, bytes;
    unsigned long long gapNs, maxGapNs, overPeriod, crossedVblank, hardwareSamples, unknownVblank;
    unsigned long long lastBeginNs, lastEndNs, lastSurfaceId, periodNs, timerSamples;
    unsigned long long totalHistogram[NVRM_PRESENT_BUCKETS], copyHistogram[NVRM_PRESENT_BUCKETS];
    unsigned long long gapHistogram[NVRM_PRESENT_BUCKETS], readyHistogram[NVRM_PRESENT_BUCKETS];
};
struct NVRMPresentStats {
    unsigned long long version, headCount, eventCapacity, eventWrites;
    NVRMPresentHead heads[NVRM_PRESENT_HEADS];
    NVRMPresentEvent events[NVRM_PRESENT_EVENTS];
};
// Buckets 0..14 have inclusive upper limits of 32us * 2^i; bucket 15 is overflow.
static inline unsigned nvrmPresentBucket(unsigned long long ns) {
    unsigned i = 0; unsigned long long limit = 32000;
    while (i < NVRM_PRESENT_BUCKETS - 1 && ns > limit) { ++i; limit <<= 1; }
    return i;
}
static inline unsigned long long nvrmPresentDelta(unsigned long long begin, unsigned long long end) {
    return end >= begin ? end - begin : 0;
}
static inline void nvrmPresentRecord(NVRMPresentStats *stats, const NVRMPresentEvent &event) {
    if (event.head >= NVRM_PRESENT_HEADS) return;
    NVRMPresentHead &h = stats->heads[event.head]; ++h.attempts;
    if (event.outcome == 1) ++h.copied;
    else if (event.outcome == 2) ++h.flips;
    else if (event.outcome == 3) ++h.held;
    else if (event.outcome == 4) ++h.empty;
    else ++h.failed;
    h.totalNs += event.totalNs; if (event.totalNs > h.maxTotalNs) h.maxTotalNs = event.totalNs;
    ++h.totalHistogram[nvrmPresentBucket(event.totalNs)];
    h.readyNs += event.readyNs; if (event.readyNs > h.maxReadyNs) h.maxReadyNs = event.readyNs;
    ++h.readyHistogram[nvrmPresentBucket(event.readyNs)];
    if (event.outcome == 1) {
        h.copyNs += event.copyNs; if (event.copyNs > h.maxCopyNs) h.maxCopyNs = event.copyNs;
        h.bytes += event.bytes; ++h.copyHistogram[nvrmPresentBucket(event.copyNs)];
        if (h.lastBeginNs && event.beginNs >= h.lastBeginNs) {
            unsigned long long gap = event.beginNs - h.lastBeginNs;
            h.gapNs += gap; if (gap > h.maxGapNs) h.maxGapNs = gap;
            ++h.gapHistogram[nvrmPresentBucket(gap)];
        }
        if (event.beginNs >= h.lastBeginNs) h.lastBeginNs = event.beginNs;
        h.lastEndNs = event.beginNs + event.totalNs; h.lastSurfaceId = event.surfaceId;
        if (event.flags & 1) ++h.hardwareSamples; else if (event.flags & 16) ++h.timerSamples; else ++h.unknownVblank;
        if (event.flags & 2) ++h.crossedVblank;
        if (event.periodNs && event.copyNs > event.periodNs) ++h.overPeriod;
        h.periodNs = event.periodNs;
    }
    stats->events[stats->eventWrites % NVRM_PRESENT_EVENTS] = event; ++stats->eventWrites;
}
