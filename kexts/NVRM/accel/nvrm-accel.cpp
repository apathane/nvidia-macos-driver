/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "../nvrm_off.h"
#ifdef NMX_ACCEL
// NMXAccel: this accelerator built for the AMD and Intel GPUs that NMXGPU drives (see nmx_accel_names.h)
#include "nmx_accel_names.h"
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#include "../nvrm_gpuva_abi.h"
class NVMemoryMap;
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                           unsigned long long len, unsigned long long align);
static void               nvAccelGpuVaFree(IOAccelMemoryMap *map);
static bool               nvAccelFlipTo(IOAccelResource2 *res);
static bool               nvAccelCopyToScanout(IOAccelResource2 *res);
void                      nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
bool                      nvAccelResHasContent(IOAccelResource2 *res);
static void               nvAccelWriteStamp(int index, unsigned int value);
#ifdef NMX_ACCEL
#define NV_LOG_TAG "NMXAccel: "
#else
#define NV_LOG_TAG "NVAccel: "
#endif
#define ALOG(fmt, ...) do { kprintf(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); IOLog(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); } while (0)
#include <libkern/c++/OSSymbol.h>
#include "nvrm_vram_abi.h"
#include "../nvrm_present_diag.h"
#include "../nvrm_present_copy.h"
#include "../nvrm_cache_probe.h"
#include <IOKit/IODeviceMemory.h>
#include "tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables
#include "nvaccel_stubs.h"
#include "nvkms-kapi.h"
#include <sys/sysctl.h>
#include <libkern/OSAtomic.h>
extern "C" {   // kern/thread_call.h and kern/clock.h clash with the family headers here, so the KPIs are declared by hand
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
}
static volatile SInt64 gCntVmAlloc, gCntVmAllocBytes, gCntVmDealloc, gCntVmDeallocBytes, gCntVmRefused,
                       gCntResNew, gCntResFree, gCntMapCommit, gCntMapRelease, gCntMapFree;
static void nvaccelInstallSysctl(void);
static int gNvAccelArmed = 0;
// 1 once the headless check restored the plugin name without arming (no NVIDIA display head). The Metal plugin reads it:
// a headless GPU hands frames to another GPU's WindowServer through system memory, so IOSurfaces must be copied out.
static int gNvHeadless = 0;
#ifdef NMX_ACCEL
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = AMD/Intel Metal device for apps; frames are copied to the framebuffer WindowServer draws");
#else
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = no NVIDIA display: Metal device for apps only, WindowServer on the GPU that owns the display");
#endif
// 4 NVIDIA heads (nvfbheads=4) + 1 for an Intel built-in panel (NMIntelFB, IOFBDependentIndex 4): WindowServer composites
// that panel on the NVIDIA card and the display-pipe copy path below writes each frame into the Intel scan-out surface.
static const unsigned kNvMaxFB = 5;
static unsigned gNvHeadsPublished = 0, gNvHeadsRegistered = 0;
static unsigned gNvHeadsPiped = 0;
// The Intel built-in panel (head 4) joins only when this is 1. Off by default while the path is proven: a write at run
// time (then debug.nvaccelfb=3 and a WindowServer restart) can be undone by a reboot, so a hang never needs hands
// (the first virtual-panel test froze the machine at boot and needed a power cycle).
static int gNvIgpu = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_igpu, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvIgpu, 0,
           "1 = give an Intel built-in panel (NMIntelFB, head 4) a display pipe on this accelerator");

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_ACCEL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMXAccel, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVAccel, "0.1.8", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class NVEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVEventMachine)
    NM_TAHOE_FWD(NVEventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        nvAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOG("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOG("EM eventTimeout(%d) — ignored, we do not use family channels", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(NVEventMachine, IOAccelEventMachineFast2)
static void (*gEnableAccel)(const char *why) = nullptr;
static int gNvScanout = 0;
static int gNvAperture = 0;
class NVDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(NVDisplayMachine)
    NM_TAHOE_FWD(NVDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(NVDisplayMachine, IOAccelLegacyDisplayMachine)
#define NV_PRIVATE_VA_BASE  0x800000000000ull
#define NV_PRIVATE_VA_SIZE  0x000010000000ull
class NVTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVTask)
    NM_TAHOE_FWD(NVTask)
public:
    IORangeAllocator *fVa = nullptr;
    unsigned fAllocLogged = 0, fPrivate = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        const unsigned long long align = map ? *(const volatile unsigned long long *)((const unsigned char *)map + 0x80) : 0;
        unsigned long long got = nvAccelGpuVaAlloc((IOAccelMemoryMap *)map, len, align);
        if (!got && fAllocLogged < 12)
            ALOG("NVTask::allocate: RM refused %llu bytes (align %llu) -- no GPU address", len, align);
        return got;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        (void)addr;
        nvAccelGpuVaFree((IOAccelMemoryMap *)map);
    }
};
OSDefineMetaClassAndStructors(NVTask, IOAccelTask)
class NVMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(NVMemoryMap)
    NM_TAHOE_FWD(NVMemoryMap)
public:
    IOAccelMemory     *fMem       = nullptr;
    unsigned int       fHVirt     = 0;
    unsigned int       fHRmMemory = 0;
    unsigned long long fGpuVa     = 0;
    unsigned long long fVaLen     = 0;
    bool               fMapped    = false;
    unsigned int fCommits = 0;
    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m,
              unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o)) return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fMem) { fMem->release(); fMem = nullptr; }
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(NVMemoryMap, IOAccelMemoryMap)

#define NV_CARRIERS 32
class IOAccelNVRMCarrier : public IOService {
    OSDeclareDefaultStructors(IOAccelNVRMCarrier)
};
OSDefineMetaClassAndStructors(IOAccelNVRMCarrier, IOService)

class NVAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVAccel)
    NM_TAHOE_FWD(NVAccel)
    IOAccelNVRMCarrier *fCarriers[NV_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    IOBufferMemoryDescriptor *fStamps = nullptr; IORangeAllocator *fRanges[32] = {};
    uint64_t  getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) { fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096); if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); } }
        if (p) *p = fStamps ? 4096 : 0;
        ALOG("getStampMemory(%p) -> descriptor %p (%u stamps, 16 KB)", p, fStamps, p ? *p : 0); return (uint64_t)fStamps; }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { NVEventMachine *m = OSTypeAlloc(NVEventMachine); ALOG("newEventMachine -> %p (family inits it)", m); return m; }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        NVTask *t = OSTypeAlloc(NVTask); bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok); if (t && !ok) { t->release(); t = nullptr; } return t; }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING to touch it", p);
            return;
        }
        ALOG("populateAccelConfig(%p) — what the FAMILY pre-filled, before we write a byte:", p);
        for (unsigned i = 0; i < 0x70; i += 8)
            ALOG("   cfg+0x%02x = 0x%016llx", i, *(const uint64_t *)(p + i));

        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x40004000u;
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        *(uint32_t *)(p + 0x28) = kNvMaxFB;
        ALOG("populateAccelConfig(%p) — filled: AccelCaps=15, flags=0x30844300, maxtex=16384x16384, maxfb=%u written (cfg+0x2c left %u)",
             p, kNvMaxFB, *(const uint32_t *)(p + 0x2c));
    }
    bool      configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("configureDevice(%p) %04x:%04x — RM owns the card (NVRM.kext); nothing to do here yet", pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0); return true; }
    void      teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        NVDisplayPipe *p = OSTypeAlloc(NVDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED - the family will store this NULL unchecked"); return NULL; }
        ALOG("newDisplayPipe -> %p", p); return p; }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE { NVDisplayMachine *d = OSTypeAlloc(NVDisplayMachine);
        fDM = d;
        ALOG("newDisplayMachine -> %p", d); return d; }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { NVGLContext *c = OSTypeAlloc(NVGLContext); ALOG("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { NVCLContext *c = OSTypeAlloc(NVCLContext); ALOG("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { NVSurface *o = OSTypeAlloc(NVSurface); ALOG("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { NV2DContext *c = OSTypeAlloc(NV2DContext); ALOG("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { NVVideoContext *c = OSTypeAlloc(NVVideoContext); ALOG("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { NVSysMemory *m = OSTypeAlloc(NVSysMemory); ALOG("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { NVVidMemory *m = OSTypeAlloc(NVVidMemory); ALOG("newVidMemory -> %p", m); return m; }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { NVResource *r = OSTypeAlloc(NVResource); OSAddAtomic64(1, &gCntResNew); ALOG("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { NVMemoryMap *m = OSTypeAlloc(NVMemoryMap); ALOG("newMemoryMap -> %p", m); return m; }

    NVDisplayMachine *fDM = nullptr;
    IOPCIDevice *fPCI = nullptr;
    IONotifier *fFBNote = nullptr;
    IONotifier *fIntelNote = nullptr;
    static NVAccel *gAccel;
    IOService *fFB = nullptr;
    IOService *volatile fFBs[kNvMaxFB] = {};
    unsigned fFBRegistered = 0;
    IOLock *fRegLock = nullptr;
    OSObject *fMetalPlugin = nullptr;
    static bool fbPublished(void *target, void *ref, IOService *newService, IONotifier *note) {
        (void)ref; (void)note;
        NVAccel *me = (NVAccel *)target;
        if (!me || !newService) return true;
        unsigned idx = 0;
        OSNumber *num = OSDynamicCast(OSNumber, newService->getProperty("IOFBDependentIndex"));
        if (num) idx = num->unsigned32BitValue();
        const char *what = "kept";
        if (idx >= kNvMaxFB) what = "OUT OF RANGE -- this head will get no pipe";
        else {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFBs[idx])) OSBitOrAtomic(1u << idx, &gNvHeadsPublished);
            else { newService->release(); what = "DUPLICATE -- the first one keeps the slot"; }
        }
        newService->retain();
        if (idx == 0 && me->fFBs[0] == newService) {
            IOService *old;
            do { old = me->fFB; } while (!OSCompareAndSwapPtr(old, newService, (void *volatile *)&me->fFB));
            if (old == newService) newService->release();
            else if (old) {
                bool slotted = false;
                for (unsigned j = 0; j < kNvMaxFB; j++) if (me->fFBs[j] == old) slotted = true;
                ALOG("fFB: head 0 (%s) displaced %s, which published first (%s)", newService->getName(), old->getName(),
                     slotted ? "its extra reference returned" : "NOT slotted: keeping its reference rather than risk a reader");
                if (slotted) old->release();
            }
        // only head 0 may become the primary framebuffer: an Intel panel (index 4) publishing first became fFB (10-10 log:
        // "head 0 displaced NMIntelFB, which published first"), and on a laptop with nothing on the NVIDIA ports it stays
        } else if (idx != 0 || !OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) newService->release();
        unsigned before = me->fDM ? me->fDM->getFramebufferCount() : 0xFFFFFFFFu;
        ALOG("%s published -- display machine framebuffer count = %u (index %u %s)", newService->getName(), before, idx, what);
        if (gNvAccelArmed)
            ALOG("  index %u published AFTER the arm: it gets a pipe only when debug.nvaccelfb=3 is written again, "
                 "and WindowServer composites it only after a WindowServer restart", idx);
        else
            ALOG("  to register it:  sudo sysctl -w debug.nvaccelfb=1   (2 = re-run the family's own sweep)");
        return true;
    }
    // Headless GPU (laptops whose panel and outputs are all on the Intel or AMD GPU: issues #76, #106). NVKMS finds no
    // display, so no NVRMFB head ever publishes and the withheld MetalPluginName was never restored: Metal never offered
    // the card, although NVRM and NVK run it fine (#106: vkCreateInstance enumerates the GTX 1660 Ti). Apple's own
    // two-GPU MacBook Pros work this way: the integrated GPU drives every display and WindowServer, and the discrete GPU
    // is a second Metal device that apps choose and macOS copies their frames from. So when no head has published by the
    // deadline, the plugin name goes back on WITHOUT arming: apps get the NVIDIA device, and WindowServer (allow-listed
    // "!WindowServer" = only when armed) keeps the GPU that owns the display. Nothing here touches a display pipe or a
    // mode change. Boot-arg -nvaccelnoheadless keeps the old behaviour.
    static constexpr uint32_t kHeadlessDeadlineSec = 60;   // chosen: NVRMFB heads publish within ~20 s of start (#76 log)
    struct thread_call *fHeadlessCall = nullptr;
    void armHeadlessCheck() {
        int off = 0;
        if (PE_parse_boot_argn("-nvaccelnoheadless", &off, sizeof off)) { ALOG("headless check: off (-nvaccelnoheadless)"); return; }
        fHeadlessCall = thread_call_allocate(&NVAccel::headlessCheck, this);
        if (!fHeadlessCall) { ALOG("headless check: thread_call_allocate FAILED -- a GPU with no display stays hidden from Metal"); return; }
        uint64_t when = 0;
        clock_interval_to_deadline(kHeadlessDeadlineSec, kSecondScale, &when);
        thread_call_enter_delayed(fHeadlessCall, when);
        ALOG("headless check: armed for %u s from now", kHeadlessDeadlineSec);
    }
    static void headlessCheck(void *p0, void *) {
        NVAccel *me = (NVAccel *)p0;
        if (!me || me != gAccel) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        const char *why = me->fFB ? "a display head published" : !me->fMetalPlugin ? "no plugin name to restore"
                        : me->getProperty("MetalPluginName") ? "the plugin name is already back" : nullptr;
        if (why) {
            ALOG("headless check: not headless (%s) -- nothing to do", why);
        } else {
            // name the GPU that owns the display, so a log says which driver WindowServer is on
            char owner[64] = "none found";
            if (OSIterator *it = getMatchingServices(serviceMatching("IOFramebuffer"))) {
                while (OSObject *o = it->getNextObject()) {
                    IOService *s = OSDynamicCast(IOService, o);
                    if (s && !s->metaCast("NVRMNVDAFramebuffer")) { strlcpy(owner, s->getMetaClass()->getClassName(), sizeof owner); break; }
                }
                it->release();
            }
            me->setProperty("MetalPluginName", me->fMetalPlugin);
            me->setProperty("NVAccelHeadless", kOSBooleanTrue);
            gNvHeadless = 1;
            ALOG("headless check: no NVIDIA display head after %u s -- MetalPluginName restored WITHOUT arming: apps get "
                 "the NVIDIA Metal device, WindowServer stays on the display's GPU (%s)", kHeadlessDeadlineSec, owner);
        }
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
    }
    void deliverModeChange() {
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        ALOG("  delivering display_mode_will_change/did_change (nest %u, pipes %u)",
             *(const volatile uint32_t *)(dm + 0x10c), *(const volatile uint32_t *)(dm + 0x108));
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
        reportPipeGates("after deliverModeChange");
    }
    void reportPipeGates(const char *when) {
        uint8_t accelFlags = *(const volatile uint8_t *)((const uint8_t *)this + 0xc78);
        ALOG("  GATES %s: accel+0xc78 = 0x%02x (enabled bit 0x2 %s)",
             when, accelFlags, (accelFlags & 0x2) ? "SET" : "CLEAR <- kIOReturnNotReady");
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  GATES %s: implausible pipe count %u -- not walking it", when, n); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) { ALOG("  GATES %s: pipe %u is NULL", when, i); continue; }
            ALOG("  GATES %s: pipe %u active(+0x298) = %u, wsaaDefer(+0x282) = %u",
                 when, i, *(const volatile uint8_t *)(p + 0x298), *(const volatile uint8_t *)(p + 0x282));
        }
    }
    unsigned registerPendingLocked(const char *why) {
        unsigned added = 0;
        for (unsigned i = 0; i < kNvMaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!fb) {
                // The Intel panel (index 4, NMIntelFB) does not wait for NVIDIA heads that never publish: a machine with one
                // monitor publishes only head 0, and the walk stopped there, so the Intel head got no pipe (measured 10-10,
                // published 0x11 registered 0x1). Pipes are found by their framebuffer pointer, not their number.
                if (gNvIgpu && i < 4 && ((gNvHeadsPublished >> 4) & 1)) { i = 3; continue; }
                if (gNvHeadsPublished >> i)
                    ALOG("  %s: index %u has not published (published 0x%x): stopping, so no head above it gets a pipe "
                         "this walk", why, i, gNvHeadsPublished);
                break;
            }
            if (fFBRegistered & (1u << i)) continue;
            if (i == 4 && !gNvIgpu) break;   // the Intel panel waits for debug.nvaccel_igpu=1
            unsigned b = fDM->getFramebufferCount();
            ALOG("  %s: calling found_framebuffer(%s index %u) -- count before %u", why, fb->getName(), i, b);
            fDM->found_framebuffer((IOFramebuffer *)fb);
            unsigned c = fDM->getFramebufferCount();
            fFBRegistered |= 1u << i;
            gNvHeadsRegistered = fFBRegistered;
            if (c != b + 1) {
                ALOG("  %s: index %u -- count went %u -> %u; the family did not add it (already known, or the cap "
                     "accel+0xcb0 = %u refused it). Stopping here so no later head takes this one's pipe number.",
                     why, i, b, c, *(const volatile uint32_t *)((const uint8_t *)this + 0xcb0));
                break;
            }
            uint8_t *dm = (uint8_t *)fDM;
            void *pipe = (b < 16) ? *(void *const volatile *)(dm + 0x88 + 8 * b) : nullptr;
            if (!pipe) {
                *(volatile uint32_t *)(dm + 0x108) = b;
                ALOG("  %s: index %u -- the family stored a NULL pipe in slot %u (0xe3b5, unchecked) and counted it; "
                     "count put back %u -> %u so no broadcast dereferences it (0xe320). This head gets no pipe this boot.",
                     why, i, b, c, fDM->getFramebufferCount());
                break;
            }
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            if (pfb != fb) {
                ALOG("  %s: index %u -- pipe %u (%p) drives %p, not %s %p; keeping the pipe (it is real), stopping "
                     "the walk so no later head's pipe number is off by this one", why, i, b, pipe, pfb, fb->getName(), fb);
                break;
            }
            ALOG("  %s: index %u -> pipe %u (%p), count %u", why, i, b, pipe, c);
            gNvHeadsPiped |= 1u << i;
            added++;
        }
        return added;
    }
    void onCountGrewLocked() {
        deliverModeChange();
        gNvAccelArmed = 1;
        if (fMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fMetalPlugin);
            ALOG("  MetalPluginName restored -- a NEW process that asks Metal for this accelerator "
                 "can now load the plugin (already-running ones will not)");
        }
        if (!getProperty("IOGLBundleName")) {
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
            ALOG("  IOGLBundleName=AppleMetalOpenGLRenderer -- NEW GL processes get a hardware renderer over Metal");
        }
        ALOG("  debug.nvaccelfb now reads 1 -- armed-only Metal clients may have a device THIS BOOT ONLY "
             "(heads published 0x%x, registered 0x%x, piped 0x%x)", gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped);
    }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        registerFramebufferLocked(how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    bool registerFramebufferLocked(int how) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        if (!fFB) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        unsigned before = fDM->getFramebufferCount();
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            registerPendingLocked("sysctl");
        }
        unsigned after = fDM->getFramebufferCount();
        ALOG("  framebuffer count after = %u", after);
        if (after > before) { onCountGrewLocked(); return true; }
        return false;
    }
    const OSSymbol *fSymAlloc = nullptr, *fSymFree = nullptr;
    IOReturn askFramebufferForVram(struct NVRMVramRequest *r, bool alloc) {
        if (!fFB) return kIOReturnNotAttached;
        if (!fSymAlloc) fSymAlloc = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_ALLOC);
        if (!fSymFree)  fSymFree  = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_FREE);
        const OSSymbol *fn = alloc ? fSymAlloc : fSymFree;
        if (!fn) return kIOReturnNoMemory;
        return fFB->callPlatformFunction(fn, false, r, nullptr, nullptr, nullptr);
    }
    IOReturn askFramebufferForScanout(struct NVRMVramRequest *r, IOService *fb = nullptr) {
        if (!fb) fb = fFB;
        if (!fb) return kIOReturnNotAttached;
        if (!fSymScanout) fSymScanout = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_SCANOUT);
        return fb->callPlatformFunction(fSymScanout, false, r, nullptr, nullptr, nullptr);
    }
    const OSSymbol *fSymScanout = nullptr;
    IOService *pipeFramebuffer(const void *pipe, unsigned *idx) {
        if (!pipe) return nullptr;
        IOService *fb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
        if (!fb) return nullptr;
        for (unsigned i = 0; i < kNvMaxFB; i++)
            if (fFBs[i] == fb) { if (idx) *idx = i; return fb; }
        return nullptr;
    }
    IOService *fNVRM = nullptr;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        if ((type & 0xFFFF0000u) != 0x4E560000u) {
            IOReturn fr = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (fUcLogged < 40) { fUcLogged++;
                ALOG("newUserClient(type %u / 0x%x) -> family returned 0x%x, client %s",
                     (unsigned)type, (unsigned)type, (unsigned)fr,
                     (handler && *handler) ? (*handler)->getMetaClass()->getClassName() : "(none)"); }
            return fr;
        }
#ifdef NMX_ACCEL
        // NVIDIA RM clients are NVK's; RADV and ANV talk to NMXGPU's DRM device nodes instead
        ALOG("newUserClient(0x%x): an NVIDIA RM client type on an AMD/Intel accelerator -- refused", type);
        return kIOReturnUnsupported;
#endif

        if (!fNVRM) {
            OSDictionary *m = serviceMatching("NVRM");
            if (m) fNVRM = waitForMatchingService(m, 100ULL * 1000 * 1000);
        }
        if (!fNVRM) { ALOG("newUserClient(0x%x): no NVRM service", type); return kIOReturnNotAttached; }

        const OSSymbol *fnSym = OSSymbol::withCStringNoCopy("nvNewAccelClient");
        if (!fnSym) return kIOReturnNoMemory;
        IOUserClient *uc = nullptr;
        IOReturn r = fNVRM->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                 (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
        fnSym->release();
        if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): NVRM refused (0x%x)", type, r); return r ? r : kIOReturnNoResources; }

        IOService *carrier = nullptr;
        for (unsigned i = 0; i < NV_CARRIERS && !carrier; i++)
            carrier = fCarriers[(fCarrierNext + i) % NV_CARRIERS];
        fCarrierNext++;
        if (!carrier) {
            ALOG("newUserClient(0x%x): NO CARRIER available -- refusing rather than attaching to the "
                 "accelerator, which sleeps at ~1019 children", type);
            uc->release(); return kIOReturnNoResources;
        }
        if (!uc->attach(carrier))  { ALOG("newUserClient(0x%x): attach failed", type); uc->release(); return kIOReturnInternalError; }
        if (!uc->start(carrier))   { ALOG("newUserClient(0x%x): start failed", type); uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
        ALOG("newUserClient(0x%x) -> %s on the ACCELERATOR (rm type %u)", type, uc->getMetaClass()->getClassName(), type & 0xFFFFu);
        *handler = uc;
        return kIOReturnSuccess;
    }
    static void triggerFromSysctl(int how) {
        if (!gAccel) { ALOG("sysctl debug.nvaccelfb=%d but no NVAccel is running", how); return; }
        if (how >= 4) gNvAperture = 1;
        if (how >= 3) {
            gNvScanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (gAccel->fRegLock) IOLockLock(gAccel->fRegLock);
            if (gAccel->registerFramebufferLocked(1))
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                     "change -- not delivering it twice", how);
            else {
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
                gAccel->deliverModeChange();
            }
            if (gAccel->fRegLock) IOLockUnlock(gAccel->fRegLock);
            return;
        }
        gAccel->registerFramebuffer(how);
    }
    static void enableFromHook(const char *why) {
        if (!gAccel) return;
        gAccel->enableAccelerator();
        gAccel->fEnableCount++;
        ALOG("enableAccelerator() from %s (call #%u)", why, gAccel->fEnableCount);
        gAccel->setProperty("NVAccelEnabledFrom", why);
        gAccel->setProperty("NVAccelEnableCount", gAccel->fEnableCount, 32);
    }
    unsigned fEnableCount = 0;
    unsigned fUcLogged = 0;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const unsigned cls = pci->configRead8(0x0B), sub = pci->configRead8(0x0A);
            const unsigned ven = pci->configRead16(0x00), dev = pci->configRead16(0x02);
#ifdef NMX_ACCEL
            if (cls != 0x03 || (ven != 0x1002 && ven != 0x8086)) {
#else
            if (cls != 0x03 || (sub != 0x00 && sub != 0x02) || ven != 0x10de || dev < 0x0020) {
#endif
                ALOG("%04x:%04x class %02x.%02x is not an NVIDIA display device we support - not claiming",
                     ven, dev, cls, sub);
                return NULL;
            }
            ALOG("NVIDIA display device %04x:%04x class %02x.%02x - claiming", ven, dev, cls, sub);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        { if (nvrm_driver_off()) return false; }
#ifdef NMX_ACCEL
        // NMXGPU decides which AMD/Intel GPUs are driven (nmxamd=1 / nmxintel=1 / nullmoth-gpu); this accelerator only
        // starts on a device it has claimed, so a GPU macOS drives itself never gets a second accelerator
        // No waiting here: NMXGPU attaches from a thread once the disks with its firmware are mounted, and mounting waits
        // for matching to settle - a start() that blocked matching for the claim would hold the boot at the logo. NMXGPU
        // re-runs matching on the device (registerService) after it sets nmxgpu-claimed, which offers it here again.
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            if (pci->getProperty("nmxgpu-claimed") != kOSBooleanTrue) {
                ALOG("NMXGPU has not claimed %04x:%04x yet - not starting now; offered again when it does",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NMXGPU drives %04x:%04x - starting", pci->configRead16(0x00), pci->configRead16(0x02));
        }
#else
        uint32_t gate = 0;
        if (!PE_parse_boot_argn("nvaccel", &gate, sizeof gate) || !gate) { ALOG("boot-arg nvaccel=1 absent: NOT starting (boot-loop guard)"); return false; }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            unsigned waited = 0;
            while (pci->getProperty("nvrm-claimed") != kOSBooleanTrue && waited < 300) { IOSleep(100); waited++; }
            if (pci->getProperty("nvrm-claimed") != kOSBooleanTrue) {
                ALOG("NVRM never claimed %04x:%04x in 30 s - NOT starting the accelerator on a GPU with no RM",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NVRM owns %04x:%04x (waited %u ms) - starting", pci->configRead16(0x00), pci->configRead16(0x02), waited * 100);
        }
#endif
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            registerService(); ALOG("registerService() called — Metal can now find MetalPluginName=%s", "NVMTLDriver");
            gAccel = this; gEnableAccel = &NVAccel::enableFromHook;
            for (unsigned i = 0; i < NV_CARRIERS; i++) {
                IOAccelNVRMCarrier *c = OSTypeAlloc(IOAccelNVRMCarrier);
                if (!c) break;
                if (!c->init()) { c->release(); break; }
                if (!c->attach(this)) { c->release(); break; }
                if (!c->start(this)) { c->detach(this); c->release(); break; }
                fCarriers[i] = c;
            }
            { unsigned n = 0; for (unsigned i = 0; i < NV_CARRIERS; i++) if (fCarriers[i]) n++;
              ALOG("user-client carriers: %u of %u ready (each holds ~1019 clients before "
                   "IOService::attach starts sleeping)", n, (unsigned)NV_CARRIERS); }
            enableFromHook("start");
            fPCI = OSDynamicCast(IOPCIDevice, provider);
            ALOG("display machine framebuffer count after the family sweep = %u", fDM ? fDM->getFramebufferCount() : 0xFFFFFFFFu);
            fRegLock = IOLockAlloc();
            ALOG("multi-head: up to %u heads, registration lock %p%s", kNvMaxFB, fRegLock,
                 fRegLock ? "" : " -- NULL: two sysctl writers could interleave a registration");
#ifdef NMX_ACCEL
            // AMD/Intel: the Metal device is for apps; WindowServer draws into NMXGPU's framebuffer, and the plugin copies
            // finished frames into the IOSurfaces it reads (the headless path NVIDIA laptops use)
            nvaccelInstallSysctl();
            gNvHeadless = 1;
            setProperty("NMXAccelHeadless", kOSBooleanTrue);
            ALOG("AMD/Intel accelerator up: Metal device for apps, frames copied to the framebuffer WindowServer draws");
            OSDictionary *fbm = nullptr;
#else
            OSDictionary *fbm = serviceMatching("NVRMNVDAFramebuffer");
#endif
            if (fbm) {
                fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbm, &NVAccel::fbPublished, this, nullptr, 0);
                fbm->release();
                ALOG("watching for NVRMFramebuffer to publish -> notifier %p", fFBNote);
                // a laptop whose built-in panel is on the Intel GPU: NMIntelFB publishes as head 4 of this accelerator
                if (OSDictionary *im = serviceMatching("NMIntelFB")) {
                    fIntelNote = addMatchingNotification(gIOFirstPublishNotification, im, &NVAccel::fbPublished, this, nullptr, 0);
                    im->release();
                    ALOG("watching for an Intel built-in panel (NMIntelFB) -> notifier %p", fIntelNote);
                }
                nvaccelInstallSysctl();
                if (getProperty("MetalPluginName")) {
                    OSObject *mp = getProperty("MetalPluginName"); if (mp) mp->retain();
                    fMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                    ALOG("MetalPluginName withheld -- nothing will load the Metal plugin until "
                         "`sysctl -w debug.nvaccelfb=1` puts it back, and a reboot takes it away again");
                    armHeadlessCheck();
                }
            } else ALOG("serviceMatching(NVRMFramebuffer) returned NULL -- cannot watch for the framebuffer");
        }
        return r;
    }
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE { ALOG("stop"); IOGraphicsAccelerator2::stop(provider); }
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVAccel, IOGraphicsAccelerator2)
NVAccel *NVAccel::gAccel = nullptr;

IOAccelMemory *NVDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res)
{
    if (!gNvScanout) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): scanout gate CLOSED -- returning NULL, "
             "which is the family's own default (sysctl debug.nvaccelfb=3 opens it)", index);
        return nullptr;
    }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !res) { ALOG("NVDisplayPipe::initFramebufferResource: no accelerator or no resource"); return nullptr; }
    unsigned head = 0;
    IOService *fb = a->pipeFramebuffer(this, &head);
    if (!fb) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): pipe %p drives framebuffer %p, which is none of ours -- "
             "refusing (never head 0's pages: that IS the bug)", index, this,
             *(void *const volatile *)((const uint8_t *)this + 0x98));
        return nullptr;
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&r, fb);
    if (ret == kIOReturnSuccess && r.kva && !r.phys) {
        // Another GPU's panel (NMIntelFB on a laptop without a MUX): its scanout is Intel memory with a CPU mapping and
        // no NVIDIA VRAM address. No vidmem may stand for it (addToAperture/getApertureMemoryDescriptor would hand
        // BAR1 offset 0 to the GPU), and none may be allocated in its place (fresh VRAM mapped here is the 10-10
        // os_map_kernel_space hang shape). NULL is the family's own default: frames reach this panel only through
        // performTransaction's copy into kva (debug.nvaccel_iop_ok_h4 counts them).
        ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u): CPU-only scanout %u x %u pitch %u (another GPU's "
             "panel) -- no framebuffer resource, frames are copied", head, index, r.width, r.height, r.pitch);
        return nullptr;
    }
    if (ret != kIOReturnSuccess || !r.phys || !r.actualSize) {
        ALOG("NVDisplayPipe::initFramebufferResource: framebuffer would not describe its scanout (0x%x)", (unsigned)ret);
        return nullptr;
    }

    OSObject *o = a->createVidMemory(nullptr, res, r.actualSize, nullptr);
    NVVidMemory *m = OSDynamicCast(NVVidMemory, o);
    if (!m) {
        ALOG("NVDisplayPipe::initFramebufferResource: createVidMemory gave %p, not an NVVidMemory", o);
        if (o) o->release();
        return nullptr;
    }
    m->fScanout = true;
    m->fPhys    = r.phys;
    m->fBytes   = r.actualSize;
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = r.phys;
    ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u, res %p) -> vidmem %p over the LIVE SCANOUT "
         "phys 0x%llx %u x %u pitch %u (%llu bytes)",
         head, index, res, m, (unsigned long long)r.phys, r.width, r.height, r.pitch,
         (unsigned long long)r.actualSize);
    return (IOAccelMemory *)m;
}

static NVVidMemory *nvres_vidmem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *(OSObject **)((unsigned char *)r + 0x88);
    return OSDynamicCast(NVVidMemory, o);
}

bool NVResource::addToAperture()
{
    NVVidMemory *m = nvres_vidmem(this);
    if (fApLogged < 4) { fApLogged++;
        ALOG("NVResource::addToAperture -> true (vidmem %p phys 0x%llx %llu bytes, already in BAR1)",
             m, m ? m->fPhys : 0ull, m ? m->fBytes : 0ull); }
    return m && m->fPhys && m->fBytes;
}

void NVResource::removeFromAperture() { }

void *NVResource::getApertureMemoryDescriptor(unsigned long long *a0)
{
    if (a0) *a0 = 0;
    if (!gNvAperture) {
        if (fApLogged < 4) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: aperture gate CLOSED -- returning nullptr "
                 "(sysctl debug.nvaccelfb=4 opens it)"); }
        return nullptr;
    }
    if (fApDesc) return fApDesc;
    NVVidMemory *m = nvres_vidmem(this);
    if (!m || !m->fPhys || !m->fBytes) {
        if (fApLogged < 8) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: no BAR1-backed vidmem at +0x88 (mem %p)", m); }
        return nullptr;
    }
    IODeviceMemory *d = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    if (!d) { ALOG("NVResource::getApertureMemoryDescriptor: IODeviceMemory::withRange(0x%llx, %llu) FAILED",
                   m->fPhys, m->fBytes); return nullptr; }
    fApDesc = d;
    if (fApLogged < 8) { fApLogged++;
        ALOG("NVResource::getApertureMemoryDescriptor -> %p over BAR1 phys 0x%llx (%llu bytes), offset 0",
             d, m->fPhys, m->fBytes); }
    return fApDesc;
}

static void nvSVForgetResource(NVResource *resource);

void NVResource::free()
{
    nvSVForgetResource(this);
    if (fApDesc) { fApDesc->release(); fApDesc = nullptr; }
    OSAddAtomic64(1, &gCntResFree);
    IOAccelResource2::free();
}

// 1 = a surface keeps its VRAM across the family's deallocPhysical/allocPhysical (see deallocPhysical).
static int gVmSticky = 1;
static SInt64 gCntVmParked, gCntVmUnparked, gCntVmParkReleased;

bool NVVidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) { ALOG("NVVidMemory::allocPhysical(%p): scanout with no address", this); return false; }
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
        ALOG("NVVidMemory::allocPhysical(%p): SCANOUT phys 0x%llx (%llu bytes) -- already backed, nothing allocated",
             this, fPhys, fBytes);
        return true;
    }
    if (fRmHandle) {
        ALOG("NVVidMemory::allocPhysical(%p): already backed by 0x%llx (%llu bytes) -- false, as AMD's does",
             this, fPhys, fBytes);
        return false;
    }
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
    if (!want) { ALOG("NVVidMemory::allocPhysical(%p): length at +0x40 is 0 -- nothing to back", this); return false; }
    if (!NVAccel::gAccel) { ALOG("NVVidMemory::allocPhysical(%p): no accelerator", this); return false; }
    if (fParkHandle) {
        if (fParkBytes >= want) {
            fRmHandle = fParkHandle; fKva = fParkKva; fPhys = fParkPhys; fBytes = fParkBytes; fAllocationCookie = fParkCookie;
            fParkHandle = nullptr; fParkKva = nullptr; fParkPhys = fParkBytes = fParkCookie = 0;
            *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
            OSAddAtomic64(1, &gCntVmUnparked);
            return true;
        }
        releaseParked();   // the resource grew: its old VRAM cannot hold it
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = NVAccel::gAccel->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        OSAddAtomic64(1, &gCntVmRefused);
        ALOG("NVVidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x)",
             this, want, (unsigned)ret);
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
    OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)fBytes, &gCntVmAllocBytes);
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
    ALOG("NVVidMemory::allocPhysical(%p): %llu bytes of REAL VRAM -- phys 0x%llx kva %p [BAR1 %llu]",
         this, fBytes, fPhys, fKva, r.mappedTotal);
    return true;
}

void NVVidMemory::deallocPhysical()
{
    if (fScanout) {
        ALOG("NVVidMemory::deallocPhysical(%p): SCANOUT -- not ours to free, only forgetting it", this);
        fPhys = 0; fBytes = 0; fScanout = false;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return;
    }
    if (!fRmHandle) return;
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)fBytes, &gCntVmDeallocBytes);
    // The family frees a surface's VRAM after copying it back for the CPU, and backs the surface with new VRAM when the
    // GPU needs it again. The Metal plugin imported the old VRAM once and keeps drawing into it, so from then on the
    // display and every copy-back read new, never-written VRAM: black frames, and windows black until a drag made
    // WindowServer import the surface again. Parked instead, this VRAM is what the next allocPhysical hands back, so the
    // surface keeps the memory the plugin draws into. It is released when the resource goes away or outgrows it.
    if (gVmSticky) {
        releaseParked();
        fParkHandle = fRmHandle; fParkKva = fKva; fParkPhys = fPhys; fParkBytes = fBytes; fParkCookie = fAllocationCookie;
        OSAddAtomic64(1, &gCntVmParked);
    } else {
        releaseVram(fRmHandle, fKva, fBytes, fAllocationCookie);
    }
    fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
}

void NVVidMemory::releaseVram(void *handle, void *kva, unsigned long long bytes, unsigned long long cookie)
{
    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.handle = handle; r.kva = kva; r.actualSize = bytes; r.allocationCookie = cookie;
    if (NVAccel::gAccel) NVAccel::gAccel->askFramebufferForVram(&r, false);
}

void NVVidMemory::releaseParked()
{
    if (!fParkHandle) return;
    releaseVram(fParkHandle, fParkKva, fParkBytes, fParkCookie);
    fParkHandle = nullptr; fParkKva = nullptr; fParkPhys = fParkBytes = fParkCookie = 0;
    OSAddAtomic64(1, &gCntVmParkReleased);
}

void NVVidMemory::free()
{
    if (fRmHandle && !fScanout) {
        releaseVram(fRmHandle, fKva, fBytes, fAllocationCookie);
        fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    }
    releaseParked();
    IOAccelVidMemory::free();
}

class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
};

static int    gNvIop = 0;
static SInt64 gCntIopOk, gCntIopFail, gCntIopEmpty, gCntIopSrcRes, gCntIopSrcSurf, gCntIopBlank;
static SInt64 gCntIopOkHead[kNvMaxFB];
#define NVIOP_LOG_MAX 40
static unsigned gIopLogs = 0;
#define NVIOPLOG(fmt, ...) do { if (gIopLogs < NVIOP_LOG_MAX) { gIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define NVIOP_FAIL(rc, fmt, ...) do { OSAddAtomic64(1, &gCntIopFail); NVIOPLOG(fmt, ##__VA_ARGS__); return (rc); } while (0)

void NVDisplayPipe::iopDropSources()
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        NVIopSrc *e = &fIopSrc[i];
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
    }
}

void NVDisplayPipe::free()
{
    iopDropSources();
    IOAccelLegacyDisplayPipe::free();
}

const void *NVDisplayPipe::iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need)
{
    for (unsigned i = 0; i < kIopSrcSlots; i++)
        if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map)
            return (fIopSrc[i].map->getLength() >= need) ? (const void *)fIopSrc[i].map->getVirtualAddress() : nullptr;
    if (md->getLength() < need) return nullptr;
    NVIopSrc *e = &fIopSrc[fIopNext % kIopSrcSlots]; fIopNext++;
    if (e->map) { e->map->release(); e->map = nullptr; }
    if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
    e->id = 0;
    if (md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); e->md = md; e->map = m; e->id = id;
    NVIOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u", id, (unsigned long long)m->getLength(), (fIopNext - 1) % kIopSrcSlots);
    return (const void *)m->getVirtualAddress();
}

IOReturn NVDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    return kIOReturnSuccess;
}

static bool nvAccelIopAsyncBusy(const void *pipe);
bool NVDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
    return gNvIop != 0 && !nvAccelIopAsyncBusy(this);
}

static void nvAccelSurfScanoutSync(IOSurface *s);
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head);
static bool nvStaleLooksUniform(const volatile unsigned int *src, unsigned w, unsigned h, unsigned spr);
static bool nvAccelSurfForceFresh(IOSurface *s, unsigned head);
extern struct sysctl_oid sysctl__debug_nvaccel_stale_checks, sysctl__debug_nvaccel_stale_fixed, sysctl__debug_nvaccel_stale_failed, sysctl__debug_nvaccel_stale_valid;
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe);
static bool nvAccelIopHome(IOService *fb, unsigned head);
static IOSimpleLock *gPresentDiagLock;
static const OSSymbol *gPresentTimingFn;
static int gPresentDiagEnabled = 1;
static int gPresentCopyMode = 2;
// CACHEPROBE-r1: where the last copied frame came from / went to, and mode-3 read/write split.
static volatile uint64_t gProbeSrcKva, gProbeDstKva, gProbeDstPhys;
static volatile SInt64 gProbeFrames, gProbeBytes, gProbeReadAbs, gProbeWriteAbs, gProbeBounceBusySkips;
static volatile SInt32 gProbeBounceBusy;
static uint64_t gProbeBounce[16384 / 2] __attribute__((aligned(64))); // one row up to 16384 BGRA pixels
// SCANOUTWC-r1: the scanout copy writes through a second, write-combining mapping of the same scanout pages.
// Returns that mapping with gScanoutWcLock held (release with nvScanoutWcRelease), or nullptr to use q.kva.
static volatile void *nvScanoutWcAcquire(uint64_t phys, uint64_t bytes);
static void nvScanoutWcRelease();
// DBFLIP-r1: double buffer. The boot scanout surface ("home") and one extra VRAM buffer of the same geometry ("back")
// alternate: each frame is copied into the buffer the panel is NOT showing, then the display engine flips to it at
// vblank (minPresentInterval 1, tearing off). Returns where to copy, and in *target which buffer to flip to after the
// copy (-1 = no flip: today's copy into the visible buffer). nullptr = drop this frame (previous flip never latched).
static volatile void *nvDbPrepare(IOService *fb, unsigned head, const struct NVRMVramRequest *q, int *target);
static void nvDbSubmit(void *pipe, IOService *fb, unsigned head, int target);
// ZEROCOPY-r1: front 2 = a zero-copy IOSurface is on the panel (neither home nor back).
static bool nvDbActive(unsigned head);
static void nvDbSetFront(unsigned head, int front);
static void nvZeroCopyArm(void);
static bool gIopFlipped[kNvMaxFB]; // true while a zero-copy IOSurface is on the panel
extern struct sysctl_oid sysctl__debug_nvaccel_dbflip, sysctl__debug_nvaccel_dbflip_state; // defined with the DBFLIP code
static NVRMPresentStats gPresentStats = { NVRM_PRESENT_DIAG_VERSION, NVRM_PRESENT_HEADS, NVRM_PRESENT_EVENTS };
static uint64_t nvPresentNowNs() {
    uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;
}
static bool nvPresentTiming(IOService *fb, NVRMPresentTiming *timing) {
    if (!fb) return false;
    if (!gPresentTimingFn) return false;
    timing->version = NVRM_PRESENT_DIAG_VERSION;
    return fb->callPlatformFunction(gPresentTimingFn, false, timing, nullptr, nullptr, nullptr) == kIOReturnSuccess;
}
class NVPresentTrace {
public:
    bool enabled; NVRMPresentEvent event = {}; uint64_t readyBegin = 0;
    NVRMPresentTiming before = {}, after = {}; bool beforeOk = false;
    NVPresentTrace() : enabled(gPresentDiagEnabled && gPresentDiagLock) {
        event.head = NVRM_PRESENT_HEADS; if (enabled) event.beginNs = nvPresentNowNs();
    }
    ~NVPresentTrace() {
        if (!enabled) return;
        event.totalNs = nvrmPresentDelta(event.beginNs, nvPresentNowNs());
        IOSimpleLockLock(gPresentDiagLock); nvrmPresentRecord(&gPresentStats, event); IOSimpleLockUnlock(gPresentDiagLock);
    }
    void readyStart() { if (enabled) readyBegin = nvPresentNowNs(); }
    void readyEnd() { if (enabled) event.readyNs = nvrmPresentDelta(readyBegin, nvPresentNowNs()); }
    uint64_t copyStart(IOService *fb) {
        if (!enabled) return 0;
        beforeOk = nvPresentTiming(fb, &before);
        return nvPresentNowNs();
    }
    void copyEnd(IOService *fb, uint64_t start, uint64_t bytes) {
        if (!enabled) return;
        const uint64_t end = nvPresentNowNs(); event.copyNs = nvrmPresentDelta(start, end); event.bytes = bytes;
        bool afterOk = nvPresentTiming(fb, &after);
        if (beforeOk && afterOk && before.modeEpoch == after.modeEpoch) {
            event.periodNs = before.periodNs;
            if (before.hardwareLive && after.hardwareLive) {
                event.flags |= 1; event.vblankBefore = before.vblankSequence; event.vblankAfter = after.vblankSequence;
                if (before.periodNs && start >= before.lastVblankNs) event.phaseNs = (start - before.lastVblankNs) % before.periodNs;
                if (after.vblankSequence > before.vblankSequence && after.lastVblankNs >= start && after.lastVblankNs <= end) event.flags |= 2;
            } else if (before.lastTimerVblankNs && start >= before.lastTimerVblankNs && before.periodNs && start - before.lastTimerVblankNs < 2 * before.periodNs) {
                event.flags |= 16; event.vblankBefore = before.timerVblankSequence; event.vblankAfter = after.timerVblankSequence;
                event.phaseNs = (start - before.lastTimerVblankNs) % before.periodNs;
                if (after.timerVblankSequence > before.timerVblankSequence && after.lastTimerVblankNs >= start && after.lastTimerVblankNs <= end) event.flags |= 32;
            } else event.flags |= 8;
        } else { event.flags |= 8; if (beforeOk && afterOk && before.modeEpoch != after.modeEpoch) event.flags |= 4; }

    }
};
IOReturn NVDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    NVPresentTrace trace;
    if (!gNvIop) return (IOReturn)0xe00002c7;
    if (!txn) NVIOP_FAIL(kIOReturnBadArgument, "performTransaction(NULL)");
    NVAccel *a = NVAccel::gAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(this, &head) : nullptr;
    trace.event.head = head;
    if (!fb) NVIOP_FAIL(kIOReturnNotAttached, "no accelerator, or pipe %p drives a framebuffer that is none of ours", this);

    IOSurface *s  = txn->getPlaneIOSurface(0, 0);
    OSObject  *ro = txn->getPlaneResource(0, 0);
    trace.event.surfaceId = s ? s->getSurfaceID() : 0;
    if (!s && !ro) { trace.event.outcome = 4; OSAddAtomic64(1, &gCntIopEmpty); return kIOReturnSuccess; }

    const volatile unsigned int *src = nullptr; unsigned w = 0, h = 0, pitch = 0; bool fromRes = false;
    NVResource *res = OSDynamicCast(NVResource, ro);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    static SInt32 gIopK6Logs;
    if (gIopK6Logs < 6) {
        OSIncrementAtomic(&gIopK6Logs);
        const unsigned char *kr = (const unsigned char *)res;
        ALOG("iop: K6 plane 0: surface %s id 0x%x %ux%u row %u fmt 0x%08x | resource %s w %u h %u +0xb8 %u +0xc0 %u vidmem %llu bytes kva %d scanout %d",
             s ? "yes" : "NO", s ? (unsigned)s->getSurfaceID() : 0u, s ? (unsigned)s->getWidth() : 0u, s ? (unsigned)s->getHeight() : 0u,
             s ? (unsigned)s->getBytesPerRow() : 0u, s ? (unsigned)s->getPixelFormat() : 0u,
             ro ? ro->getMetaClass()->getClassName() : "(none)",
             res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb0) : 0u, res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb2) : 0u,
             res ? *(const volatile unsigned int *)(kr + 0xb8) : 0u, res ? *(const volatile unsigned int *)(kr + 0xc0) : 0u,
             vm ? (unsigned long long)vm->fBytes : 0ull, (vm && vm->fKva) ? 1 : 0, vm ? (int)vm->fScanout : -1);
    }
    if (!s && vm && vm->fKva) {
        const unsigned char *rr = (const unsigned char *)res;
        w     = *(const volatile unsigned short *)(rr + 0xb0);
        h     = *(const volatile unsigned short *)(rr + 0xb2);
        pitch = *(const volatile unsigned int   *)(rr + 0xb8);
        if ((unsigned long long)pitch < 4ull * w) NVIOP_FAIL(kIOReturnBadArgument, "resource %ux%u: +0xb8 reads %u, which is not a row pitch (need >= %u) — not a surface buffer", w, h, pitch, 4u * w);
        if ((unsigned long long)h * pitch > vm->fBytes) NVIOP_FAIL(kIOReturnOverrun, "resource %ux%u pitch %u exceeds its %llu bytes", w, h, pitch, (unsigned long long)vm->fBytes);
        src = (const volatile unsigned int *)vm->fKva; fromRes = true;
    } else if (s && nvAccelIopFlip(fb, head, s, this)) {
        trace.event.outcome = 2;
        OSAddAtomic64(1, &gCntIopSrcSurf); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        return kIOReturnSuccess;
    } else if (s) {
        // A copied frame must come from the surface's newest pixels. Copying while they were still only in VRAM put a
        // stale, one-colour buffer on the panel every other frame: full-screen flicker, 5 of 40 flicker recordings
        // (1.13-1.14.1), nvaccel_iop_blank about half of every copied frame, never on machines whose frames all flip.
        // Such a frame now keeps the previous picture on the panel instead.
        trace.readyStart();
        const bool ready = nvAccelSurfCopyReady(s, head); trace.readyEnd();
        if (!ready) { trace.event.outcome = 3; return kIOReturnSuccess; }
        const unsigned fmt = s->getPixelFormat();
        if (fmt != 0x42475241u) NVIOP_FAIL(kIOReturnUnsupported, "surface 0x%x format 0x%08x is not BGRA", s->getSurfaceID(), fmt);
        const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
        if (!W || !H || P < 4 || W > 16384 || H > 16384 || P > 16384ull * 8) NVIOP_FAIL(kIOReturnBadArgument, "surface geometry %llux%llu pitch %llu", W, H, P);
        if (P < 4 * W || (P & 3)) NVIOP_FAIL(kIOReturnBadArgument, "surface %llux%llu: row pitch %llu is under 4*W or not word-aligned", W, H, P);
        IOMemoryDescriptor *md = s->getMemoryDescriptor();
        if (!md) NVIOP_FAIL(kIOReturnNoMemory, "surface 0x%x has no memory descriptor", s->getSurfaceID());
        src = (const volatile unsigned int *)iopMapSurface(s->getSurfaceID(), md, H * P);
        if (!src) NVIOP_FAIL(kIOReturnVMError, "surface 0x%x would not prepare/map %llu bytes", s->getSurfaceID(), H * P);
        w = (unsigned)W; h = (unsigned)H; pitch = (unsigned)P;
        // STALEGUARD-r1: a frame that samples as one colour is checked against the surface's own validity bits.
        if (nvStaleLooksUniform(src, w, h, pitch / 4) && !nvAccelSurfForceFresh(s, head)) { trace.event.outcome = 3; return kIOReturnSuccess; }
    } else {
        NVIOP_FAIL(kIOReturnUnsupported, "plane resource %p is not an NVResource with a CPU mapping, and there is no IOSurface", ro);
    }
    if (!w || !h || pitch < 4) NVIOP_FAIL(kIOReturnBadArgument, "source geometry %ux%u pitch %u", w, h, pitch);

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q, fb);
    if (ret != kIOReturnSuccess || !q.kva || q.pitch < 4 || !q.height) NVIOP_FAIL(kIOReturnNotReady, "framebuffer would not describe its scanout (0x%x kva %p pitch %u)", (unsigned)ret, q.kva, q.pitch);
    if (w != q.width || h != q.height) NVIOP_FAIL(kIOReturnBadArgument, "source %ux%u is not the scanout's %ux%u", w, h, q.width, q.height);

    // With double buffering the next DBFLIP flip itself moves the panel off a zero-copy surface (into a buffer the
    // panel is not showing), so the synchronous home flip, an extra vblank wait per transition, is not needed.
    if (!nvDbActive(head) && !nvAccelIopHome(fb, head)) {
        trace.event.outcome = 3; return kIOReturnSuccess;
    }
    nvZeroCopyArm();
    const unsigned words = (((pitch < q.pitch) ? pitch : q.pitch) & ~3u) / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    int dbTarget = -1;
    volatile void *dbDst = nvDbPrepare(fb, head, &q, &dbTarget);
    if (!dbDst) { trace.event.outcome = 3; return kIOReturnSuccess; } // counted in gCntDbDrops
    volatile void *wc = (dbDst == q.kva) ? nvScanoutWcAcquire(q.phys, (uint64_t)q.pitch * q.height) : nullptr;
    volatile unsigned int *d = (volatile unsigned int *)(wc ? wc : dbDst);
    int copyMode = gPresentCopyMode; // one policy for the entire frame
    gProbeSrcKva = (uint64_t)(uintptr_t)src; gProbeDstKva = (uint64_t)(uintptr_t)d; gProbeDstPhys = q.phys;
    const size_t rowBytes = (size_t)words * 4;
    bool bounce = false;
    if (copyMode == 3) {
        // The bounce buffer is shared by every head; a second head copying at the same time uses mode 2.
        bounce = rowBytes <= sizeof gProbeBounce && OSCompareAndSwap(0, 1, &gProbeBounceBusy);
        if (!bounce) OSAddAtomic64(1, &gProbeBounceBusySkips);
        copyMode = 2;
    }
    const uint64_t copyStartNs = trace.copyStart(fb);
    if (bounce) {
        // Same pixels reach the panel; only the cost is split into source read and scanout write.
        uint64_t readAbs = 0, writeAbs = 0;
        for (unsigned y = 0; y < h; y++) {
            const volatile unsigned int *sp = src + (unsigned long)y * spr;
            volatile unsigned int *dp = d + (unsigned long)y * dpr;
            const uint64_t t0 = mach_absolute_time();
            nvrmPresentCopyRow(gProbeBounce, sp, rowBytes, 2);
            const uint64_t t1 = mach_absolute_time();
            nvrmPresentCopyRow(dp, gProbeBounce, rowBytes, 2);
            const uint64_t t2 = mach_absolute_time();
            readAbs += t1 - t0; writeAbs += t2 - t1;
        }
        const uint64_t f0 = mach_absolute_time();
        nvrmPresentCopyFinish(); // draining posted/WC stores is charged to the write side
        writeAbs += mach_absolute_time() - f0;
        OSAddAtomic64((SInt64)readAbs, &gProbeReadAbs); OSAddAtomic64((SInt64)writeAbs, &gProbeWriteAbs);
        OSAddAtomic64((SInt64)rowBytes * h, &gProbeBytes); OSAddAtomic64(1, &gProbeFrames);
        OSCompareAndSwap(1, 0, &gProbeBounceBusy);
    } else {
        for (unsigned y = 0; y < h; y++) {
            const volatile unsigned int *sp = src + (unsigned long)y * spr;
            volatile unsigned int *dp = d + (unsigned long)y * dpr;
            nvrmPresentCopyRow(dp, sp, rowBytes, copyMode);
        }
        nvrmPresentCopyFinish();
    }
    if (wc) nvScanoutWcRelease(); // both paths above end with the fence that drains the WC buffers
    trace.copyEnd(fb, copyStartNs, (uint64_t)words * 4 * h); trace.event.outcome = 1;
    if (dbTarget >= 0) nvDbSubmit(this, fb, head, dbTarget); // pixels are complete (fenced) before the flip is queued
    OSAddAtomic64(1, fromRes ? &gCntIopSrcRes : &gCntIopSrcSurf);
    const SInt64 n = OSAddAtomic64(1, &gCntIopOk);
    const SInt64 nh = OSAddAtomic64(1, &gCntIopOkHead[head]);
    unsigned nz = 0, varied = 0; const unsigned first = src[0] & 0xffffffu;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        unsigned sx = (w * (2 * j + 1)) / 16; if (sx >= spr) sx = spr - 1;
        const unsigned v = src[(unsigned long)((h * (2 * i + 1)) / 16) * spr + sx] & 0xffffffu;
        if (v) nz++;
        if (v != first) varied++;
    }
    if (!varied) OSAddAtomic64(1, &gCntIopBlank);
    if ((n <= 3 || n == 600 || nh == 0) && gIopK6Logs < 12) {
        OSIncrementAtomic(&gIopK6Logs);
        ALOG("iop: K6 transaction #%lld (head %u #%lld) on the panel: %ux%u pitch %u from %s — 64 samples: nonzero %u varied %u", (long long)n, head, (long long)nh, w, h, pitch,
             fromRes ? "our VRAM resource" : "the IOSurface's memory", nz, varied);
    }
    return kIOReturnSuccess;
}

static int nvacceliop_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvIop;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel *a = NVAccel::gAccel;
    if (!a) return ENXIO;
    if (v) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) return ENOMEM;
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gNvIop = 1;
        ALOG("iop: debug.nvaccel_iop=1 -- capabilities published, transaction hooks OPEN");
    } else {
        gNvIop = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("iop: debug.nvaccel_iop=0 -- capabilities removed, transaction hooks answer as the base class");
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvacceliop_sysctl, "I",
            "K3: 1 = publish IOAccelDisplayPipeCapabilities and perform display-pipe transactions, 0 = behave as the base class (default)");

static int nvaccelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvAccelArmed;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel::triggerFromSysctl(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelfb_sysctl, "I",
            "register the NVIDIA framebuffer with the accelerator display machine (1 = found_framebuffer, 2 = family sweep)");
#define NVACCEL_CNT(nm, var, desc) SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&var, desc)
NVACCEL_CNT(nvaccel_vm_alloc,         gCntVmAlloc,        "NVVidMemory::allocPhysical successes");
NVACCEL_CNT(nvaccel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to NVVidMemory");
NVACCEL_CNT(nvaccel_vm_dealloc,       gCntVmDealloc,      "NVVidMemory::deallocPhysical calls with a handle");
NVACCEL_CNT(nvaccel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes of VRAM given back by NVVidMemory");
NVACCEL_CNT(nvaccel_vm_refused,       gCntVmRefused,      "allocPhysical refused by the framebuffer");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_vm_sticky, CTLFLAG_RW | CTLFLAG_LOCKED, &gVmSticky, 0, "1 = a surface keeps its VRAM across the family's dealloc/alloc (the memory the plugin draws into)");
NVACCEL_CNT(nvaccel_vm_parked,        gCntVmParked,       "VRAM kept when the family freed a surface's VRAM");
NVACCEL_CNT(nvaccel_vm_unparked,      gCntVmUnparked,     "allocations answered with the surface's kept VRAM");
NVACCEL_CNT(nvaccel_vm_park_released, gCntVmParkReleased, "kept VRAM released (resource freed or grown)");
NVACCEL_CNT(nvaccel_res_new,          gCntResNew,         "NVResource objects created");
NVACCEL_CNT(nvaccel_res_free,         gCntResFree,        "NVResource objects freed");
NVACCEL_CNT(nvaccel_map_commit,       gCntMapCommit,      "GPU page-table commits (MAP_MEMORY_DMA ok)");
NVACCEL_CNT(nvaccel_map_release,      gCntMapRelease,     "GPU page-table releases (UNMAP_ONLY)");
NVACCEL_CNT(nvaccel_map_free,         gCntMapFree,        "GPU VA ranges freed (nvAccelGpuVaFree)");
NVACCEL_CNT(nvaccel_iop_ok,           gCntIopOk,          "K3: display-pipe transactions copied to the panel");
NVACCEL_CNT(nvaccel_iop_ok_h0,        gCntIopOkHead[0],   "frames copied to head 0's panel");
NVACCEL_CNT(nvaccel_iop_ok_h1,        gCntIopOkHead[1],   "frames copied to head 1's panel");
NVACCEL_CNT(nvaccel_iop_ok_h2,        gCntIopOkHead[2],   "frames copied to head 2's panel");
NVACCEL_CNT(nvaccel_iop_ok_h3,        gCntIopOkHead[3],   "frames copied to head 3's panel");
NVACCEL_CNT(nvaccel_iop_ok_h4,        gCntIopOkHead[4],   "frames copied to head 4 (an Intel built-in panel, NMIntelFB)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_published, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadsPublished, 0,
            "bit i = head i's framebuffer has published to the accelerator");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsRegistered, 0,
            "bit i = head i's framebuffer was handed to the display machine (found_framebuffer)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_piped, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsPiped, 0,
            "bit i = head i has a display pipe that drives ITS OWN framebuffer (pipe+0x98 read back)");
static_assert(kNvMaxFB == 5, "kNvMaxFB changed: add debug.nvaccel_iop_ok_h<i> for every new head");
NVACCEL_CNT(nvaccel_iop_fail,         gCntIopFail,        "K3: transactions refused (see the capped iop: log lines)");
NVACCEL_CNT(nvaccel_iop_empty,        gCntIopEmpty,       "K3: transactions with no plane (gamma/options only)");
NVACCEL_CNT(nvaccel_iop_src_res,      gCntIopSrcRes,      "K3: frames sourced from our VRAM resource");
NVACCEL_CNT(nvaccel_iop_src_surf,     gCntIopSrcSurf,     "K3: frames sourced from the IOSurface memory");
NVACCEL_CNT(nvaccel_iop_blank,        gCntIopBlank,       "K6: frames copied whose 64 samples were all ONE colour (a blank frame)");
static SInt64 gCntIopCopySkip;
NVACCEL_CNT(nvaccel_iop_copy_skip,    gCntIopCopySkip,    "frames kept on the panel because the surface's newest pixels were not yet in system memory");
// -1 is the untouched automatic policy; an explicit zero must survive auto-arm.
static int gIopFlip = -1;
static int nvaccel_iop_flip_sysctl SYSCTL_HANDLER_ARGS {
    int value = __atomic_load_n(&gIopFlip, __ATOMIC_ACQUIRE) > 0;
    int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value != 0 && value != 1) return EINVAL;
    __atomic_store_n(&gIopFlip, value, __ATOMIC_RELEASE);
    return 0;
}
static SInt64 gCntIopFlip, gCntIopFlipHome, gCntIopFlipNo[8];
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop_flip, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_iop_flip_sysctl, "I", "1 = scan the composited IOSurface out of its own VRAM (no copy); explicit 0 disables auto-arm");
NVACCEL_CNT(nvaccel_iop_flips,        gCntIopFlip,        "frames scanned out of the surface's own VRAM (zero copy)");
NVACCEL_CNT(nvaccel_iop_flip_home,    gCntIopFlipHome,    "flips back to the boot scanout before a copied frame");
NVACCEL_CNT(nvaccel_iop_flip_lock,    gCntIopFlipNo[3],   "copied because the accelerator lock was busy");
NVACCEL_CNT(nvaccel_iop_flip_novram,  gCntIopFlipNo[4],   "copied because the surface has no RM VRAM");
NVACCEL_CNT(nvaccel_iop_flip_stale,   gCntIopFlipNo[5],   "copied because system memory holds the newest copy");
NVACCEL_CNT(nvaccel_iop_flip_refused, gCntIopFlipNo[7],   "copied because the framebuffer refused the flip");
static SInt64 gCntIopFlipHit, gCntIopFlipMiss;
NVACCEL_CNT(nvaccel_iop_flip_hit,     gCntIopFlipHit,     "swaps resolved from the cache (no accelerator lock)");
NVACCEL_CNT(nvaccel_iop_flip_miss,    gCntIopFlipMiss,    "swaps that resolved under the accelerator lock");
static int    gIopAsync = 1;
static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_async, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopAsync, 0, "1 = the swap runs on a thread call and completes through signalTransactionInterrupt (Apple's shape); 0 = the earlier synchronous flip");
NVACCEL_CNT(nvaccel_iop_async_flips,   gCntIopAsync,        "swaps completed asynchronously");
NVACCEL_CNT(nvaccel_iop_async_refused, gCntIopAsyncRefused, "async swaps NVKMS refused (that frame stayed off the panel)");
NVACCEL_CNT(nvaccel_iop_async_drop,    gCntIopAsyncDrop,    "frames dropped because a flip was still in flight (never expected)");
static int gCrcHead = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_crc_head, CTLFLAG_RW | CTLFLAG_LOCKED, &gCrcHead, 0, "b85k-crc: which head debug.nvaccel_crc reads");
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_crc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvaccel_crc_sysctl, "I",
            "b85k-crc: display CRC32 of what head nvaccel_crc_head is showing (0 = unavailable)");
static int nvaccel_present_copy_mode_sysctl SYSCTL_HANDLER_ARGS {
    int mode = gPresentCopyMode;
    int err = sysctl_handle_int(oidp, &mode, 0, req);
    if (err || !req->newptr) return err;
    if (mode < 0 || mode > 3) return EINVAL;
    gPresentCopyMode = mode;
    ALOG("PRESENTCOPY-r1: copy mode %d", mode);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_present_copy_mode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccel_present_copy_mode_sysctl, "I", "PRESENTCOPY-r1: 0 scalar32, 1 scalar64, 2 rep movsq (default), 3 CACHEPROBE-r1 bounce split (same pixels, timed read/write); all drain WC stores");
// ---- CACHEPROBE-r1 -------------------------------------------------------------------------------------------
// Runs only when written (debug.nvaccel_cacheprobe_run=1), never on a plain `sysctl -a`. Reads PAT/MTRR MSRs and walks
// the current page tables through physical reads for the last frame's source and scanout addresses.
static char gProbeText[1536] = "CACHEPROBE-r1: not run yet (sudo sysctl -w debug.nvaccel_cacheprobe_run=1)";
static IOLock *gProbeLock;
static inline uint64_t nvProbeRdmsr(uint32_t msr) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return ((uint64_t)hi << 32) | lo; }
static int nvProbePhysRead64(uint64_t pa, uint64_t *value, void *) {
    IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)pa, 8, kIODirectionOut);
    if (!md) return 1;
    const IOByteCount got = md->readBytes(0, value, 8); md->release();
    return got == 8 ? 0 : 1;
}
static bool nvProbeIsAmd() {
    uint32_t a = 0, b, c, d; __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return b == 0x68747541u && d == 0x69746e65u && c == 0x444d4163u; // "AuthenticAMD"
}
static size_t nvProbeDescribe(char *out, size_t cap, const char *label, uint64_t va, uint64_t cr3, uint64_t pat,
                              const NVRMMtrrState *m, uint64_t expectPa) {
    if (!va) return (size_t)snprintf(out, cap, "%s: no frame copied yet\n", label);
    const NVRMCacheWalk w = nvrmCacheWalk(cr3, va, nvProbePhysRead64, nullptr);
    if (!w.ok) return (size_t)snprintf(out, cap, "%s va 0x%llx: walk stopped at level %d\n", label, va, w.failStep);
    const unsigned pt = nvrmPatType(pat, w.patIndex), mt = nvrmMtrrType(m, w.pa), eff = nvrmEffectiveType(pt, mt);
    return (size_t)snprintf(out, cap, "%s va 0x%llx -> pa 0x%llx (%s page, entry 0x%llx) PAT[%u]=%s MTRR=%s EFFECTIVE=%s%s\n",
                            label, va, w.pa, w.level == 1 ? "4K" : (w.level == 2 ? "2M" : "1G"), w.entry, w.patIndex,
                            nvrmMemTypeName(pt), nvrmMemTypeName(mt), nvrmMemTypeName(eff),
                            expectPa ? ((w.pa & ~0xfffull) == (expectPa & ~0xfffull) ? " (matches scanout phys)" : " (DIFFERS from scanout phys)") : "");
}
static uint64_t nvProbeReadState(NVRMMtrrState *m, uint64_t *capOut, bool *amdOut) {
    const uint64_t pat = nvProbeRdmsr(0x277), cap = nvProbeRdmsr(0xfe);
    m->defType = nvProbeRdmsr(0x2ff); m->count = (unsigned)(cap & 0xff); if (m->count > 16) m->count = 16;
    for (unsigned i = 0; i < m->count; i++) { m->base[i] = nvProbeRdmsr(0x200 + 2 * i); m->mask[i] = nvProbeRdmsr(0x201 + 2 * i); }
    const bool amd = nvProbeIsAmd();
    if (amd) {
        m->syscfg = nvProbeRdmsr(0xc0010010);
        if (m->syscfg & (1ull << 21)) m->tom2 = nvProbeRdmsr(0xc001001d);              // TOM2 is defined only while enabled
        m->amdTom2 = nvrmAmdTom2ForcesWb(m->syscfg);
    }
    if (capOut) *capOut = cap;
    if (amdOut) *amdOut = amd;
    return pat;
}
// ---- SCANOUTWC-r1 --------------------------------------------------------------------------------------------
// The scanout is mapped by NVKMS with kIOMapWriteCombineCache, which xnu encodes as PAT index 6. This machine's
// OpenCore "Fix PAT" patch programs PAT 0x0007010600070106, where index 6 is UC- and the scanout MTRR is UC, so
// every frame is written uncached (CACHEPROBE-r1: 756 MB/s, 11 ms per 1080p frame). kIOMapWriteThruCache is encoded
// as PWT = index 1, which that PAT makes WC. The mapping is only used after the page tables and PAT/MTRR prove it is
// WC and points at exactly the scanout's physical pages; any other result keeps the NVKMS mapping (today's path).
// Other mappings of these pages (NVKMS kernel/user, NVRMFB BAR1 survey and console) are not changed. Every page must
// have MTRR UC, which turns any PAT type those mappings use into UC (or WC), so the WC alias never coexists with a
// cacheable mapping; a UC/WC alias only affects ordering, and the copy ends with MFENCE.
static int gScanoutWcEnable = 0; // PATWC-r1 made the NVKMS mapping itself WC; the alias is no longer needed
static IOLock *gScanoutWcLock;
static struct { uint64_t phys, bytes; IOMemoryDescriptor *md; IOMemoryMap *map; volatile void *kva; int verdict; } gScanoutWc;
static char gScanoutWcWhy[256] = "no scanout copied yet";
static volatile SInt64 gCntWcFrames, gCntWcFallback, gCntWcBusy, gCntWcMaps;
static void nvScanoutWcDrop() {
    if (gScanoutWc.map) gScanoutWc.map->release();
    if (gScanoutWc.md) gScanoutWc.md->release();
    gScanoutWc.map = nullptr; gScanoutWc.md = nullptr; gScanoutWc.kva = nullptr; gScanoutWc.verdict = 0;
    gScanoutWc.phys = 0; gScanoutWc.bytes = 0;
}
static bool nvScanoutWcCheck(uint64_t va, uint64_t wantPa, uint64_t cr3, uint64_t pat, const NVRMMtrrState *m, const char *where) {
    const NVRMCacheWalk w = nvrmCacheWalk(cr3, va, nvProbePhysRead64, nullptr);
    if (!w.ok) { snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "REFUSED: %s page walk stopped at level %d", where, w.failStep); return false; }
    if (w.pa != wantPa) { snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "REFUSED: %s maps pa 0x%llx, scanout is 0x%llx", where, w.pa, wantPa); return false; }
    const unsigned pt = nvrmPatType(pat, w.patIndex), mt = nvrmMtrrType(m, w.pa), eff = nvrmEffectiveType(pt, mt);
    if (!nvrmScanoutWcPageOk(pt, mt)) {
        // MTRR must be UC (so no other mapping of this page can be cacheable) and this alias must resolve to WC.
        snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "REFUSED: %s PAT[%u]=%s MTRR=%s effective %s (need MTRR UC and effective WC)",
                 where, w.patIndex, nvrmMemTypeName(pt), nvrmMemTypeName(mt), nvrmMemTypeName(eff));
        return false;
    }
    return true;
}
static void nvScanoutWcMake(uint64_t phys, uint64_t bytes) {
    gScanoutWc.phys = phys; gScanoutWc.bytes = bytes; gScanoutWc.verdict = -1; // a failed attempt is not retried for this scanout
    OSAddAtomic64(1, &gCntWcMaps);
    gScanoutWc.md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)phys, (IOByteCount)bytes, kIODirectionInOut);
    if (!gScanoutWc.md) { snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "REFUSED: no descriptor for 0x%llx+%llu", phys, bytes); return; }
    gScanoutWc.map = gScanoutWc.md->map(kIOMapWriteThruCache);
    if (!gScanoutWc.map || gScanoutWc.map->getLength() < bytes) {
        snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "REFUSED: map of 0x%llx+%llu failed", phys, bytes);
        if (gScanoutWc.map) { gScanoutWc.map->release(); gScanoutWc.map = nullptr; }
        return;
    }
    const uint64_t va = (uint64_t)gScanoutWc.map->getVirtualAddress();
    NVRMMtrrState m = {}; uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    const uint64_t pat = nvProbeReadState(&m, nullptr, nullptr);
    // Every page: one stray UC page would only be slow, but a page pointing elsewhere would corrupt memory.
    for (uint64_t off = 0; off < bytes; off += 4096) {
        if (!nvScanoutWcCheck(va + off, phys + off, cr3, pat, &m, off ? "page" : "first page")) {
            gScanoutWc.map->release(); gScanoutWc.map = nullptr; return;
        }
    }
    if (!nvScanoutWcCheck(va + bytes - 1, phys + bytes - 1, cr3, pat, &m, "last byte")) { gScanoutWc.map->release(); gScanoutWc.map = nullptr; return; }
    gScanoutWc.kva = (volatile void *)va; gScanoutWc.verdict = 1;
    snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "ACTIVE: pa 0x%llx+%llu via va 0x%llx, every page WC (PAT 0x%016llx)", phys, bytes, va, pat);
}
static volatile void *nvScanoutWcAcquire(uint64_t phys, uint64_t bytes) {
    if (!gScanoutWcEnable || !gScanoutWcLock || !phys || !bytes) { OSAddAtomic64(1, &gCntWcFallback); return nullptr; }
    if (!IOLockTryLock(gScanoutWcLock)) { OSAddAtomic64(1, &gCntWcBusy); return nullptr; } // another head is copying
    if (gScanoutWc.phys != phys || gScanoutWc.bytes != bytes) {
        nvScanoutWcDrop();
        nvScanoutWcMake(phys, bytes);
        ALOG("SCANOUTWC-r1: %s", gScanoutWcWhy);
    }
    if (gScanoutWc.verdict == 1) { OSAddAtomic64(1, &gCntWcFrames); return gScanoutWc.kva; }
    IOLockUnlock(gScanoutWcLock); OSAddAtomic64(1, &gCntWcFallback);
    return nullptr;
}
static void nvScanoutWcRelease() { IOLockUnlock(gScanoutWcLock); }
static int nvaccel_scanout_wc_sysctl SYSCTL_HANDLER_ARGS {
    int v = gScanoutWcEnable; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    if (!gScanoutWcLock) return ENXIO;
    IOLockLock(gScanoutWcLock); // waits for a copy in flight; the next frame uses the chosen path
    gScanoutWcEnable = v;
    if (!v) { nvScanoutWcDrop(); snprintf(gScanoutWcWhy, sizeof gScanoutWcWhy, "OFF by sysctl"); }
    IOLockUnlock(gScanoutWcLock);
    ALOG("SCANOUTWC-r1: %s", v ? "enabled" : "disabled");
    return 0;
}
static int nvaccel_scanout_wc_state_sysctl SYSCTL_HANDLER_ARGS {
    if (req->newptr) return EPERM;
    if (!gScanoutWcLock) return ENXIO;
    char *t = (char *)IOMalloc(512); if (!t) return ENOMEM;
    IOLockLock(gScanoutWcLock);
    snprintf(t, 512, "%s | wc frames %lld, fallback frames %lld, busy %lld, mappings made %lld",
             gScanoutWcWhy, (long long)gCntWcFrames, (long long)gCntWcFallback, (long long)gCntWcBusy, (long long)gCntWcMaps);
    IOLockUnlock(gScanoutWcLock);
    int ret = SYSCTL_OUT(req, t, strlen(t) + 1); IOFree(t, 512); return ret;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_scanout_wc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_scanout_wc_sysctl, "I", "SCANOUTWC-r1: 1 copy through a verified WC alias of the scanout (default), 0 use the NVKMS mapping");
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_scanout_wc_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_scanout_wc_state_sysctl, "A", "SCANOUTWC-r1: verdict and counters");
static int nvaccel_cacheprobe_run_sysctl SYSCTL_HANDLER_ARGS {
    int v = 0; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 1) return EINVAL;
    if (!gProbeLock) return ENXIO;
    char *t = (char *)IOMalloc(sizeof gProbeText); if (!t) return ENOMEM;
    NVRMMtrrState m = {};
    // Architectural MSRs only (PAT, MTRRCAP, MTRR_DEF_TYPE, variable MTRRs); AMD SYSCFG/TOM2 only on AuthenticAMD.
    // PAT/MTRR are programmed identically on every CPU and kernel mappings are shared, so migration is harmless.
    uint64_t cr3; __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t cap = 0; bool amd = false;
    const uint64_t pat = nvProbeReadState(&m, &cap, &amd);
    const uint64_t srcVa = gProbeSrcKva, dstVa = gProbeDstKva, dstPa = gProbeDstPhys;
    size_t n = (size_t)snprintf(t, sizeof gProbeText, "CACHEPROBE-r1 PAT 0x%016llx MTRRcap 0x%llx def 0x%llx vcnt %u %s SYSCFG 0x%llx TOM2 0x%llx (forces WB: %s)\n",
                                pat, cap, m.defType, m.count, amd ? "AMD" : "non-AMD", m.syscfg, m.tom2, m.amdTom2 ? "yes" : "no");
    if (n < sizeof gProbeText) n += nvProbeDescribe(t + n, sizeof gProbeText - n, "source", srcVa, cr3, pat, &m, 0);
    if (n < sizeof gProbeText) n += nvProbeDescribe(t + n, sizeof gProbeText - n, "scanout", dstVa, cr3, pat, &m, dstPa);
    if (n < sizeof gProbeText && dstPa)
        n += (size_t)snprintf(t + n, sizeof gProbeText - n, "scanout phys 0x%llx by MTRR alone: %s\n", dstPa, nvrmMemTypeName(nvrmMtrrType(&m, dstPa)));
    for (unsigned i = 0; i < m.count && n < sizeof gProbeText; i++)
        if (m.mask[i] & (1ull << 11)) n += (size_t)snprintf(t + n, sizeof gProbeText - n, "  var%u base 0x%llx mask 0x%llx\n", i, m.base[i], m.mask[i]);
    IOLockLock(gProbeLock); memcpy(gProbeText, t, sizeof gProbeText); gProbeText[sizeof gProbeText - 1] = 0; IOLockUnlock(gProbeLock);
    IOFree(t, sizeof gProbeText);
    ALOG("CACHEPROBE-r1: probe ran");
    return 0;
}
static int nvaccel_cacheprobe_sysctl SYSCTL_HANDLER_ARGS {
    if (req->newptr) return EPERM;
    if (!gProbeLock) return ENXIO;
    char *t = (char *)IOMalloc(sizeof gProbeText); if (!t) return ENOMEM;
    IOLockLock(gProbeLock); memcpy(t, gProbeText, sizeof gProbeText); IOLockUnlock(gProbeLock);
    int ret = SYSCTL_OUT(req, t, strlen(t) + 1); IOFree(t, sizeof gProbeText); return ret;
}
static int nvaccel_cacheprobe_split_sysctl SYSCTL_HANDLER_ARGS {
    if (req->newptr) return EPERM;
    const uint64_t frames = (uint64_t)gProbeFrames, bytes = (uint64_t)gProbeBytes;
    uint64_t rns = 0, wns = 0; absolutetime_to_nanoseconds((uint64_t)gProbeReadAbs, &rns); absolutetime_to_nanoseconds((uint64_t)gProbeWriteAbs, &wns);
    char t[256];
    // Per-frame microseconds and MB/s, integers only (no FPU state in the kernel).
    snprintf(t, sizeof t, "frames %llu bytes %llu read_us_per_frame %llu write_us_per_frame %llu read_MBps %llu write_MBps %llu busy_skips %lld",
             frames, bytes, frames ? rns / 1000 / frames : 0, frames ? wns / 1000 / frames : 0,
             rns ? bytes * 1000 / rns : 0, wns ? bytes * 1000 / wns : 0, (long long)gProbeBounceBusySkips);
    return SYSCTL_OUT(req, t, strlen(t) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_cacheprobe_run, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_cacheprobe_run_sysctl, "I", "CACHEPROBE-r1: write 1 to probe PAT/MTRR/page bits of the last source and scanout");
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_cacheprobe, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_cacheprobe_sysctl, "A", "CACHEPROBE-r1: result of the last probe");
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_cacheprobe_split, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_cacheprobe_split_sysctl, "A", "CACHEPROBE-r1: mode-3 read/write cost since boot");
static int nvaccel_present_stats_sysctl SYSCTL_HANDLER_ARGS {
    if (req->newptr) return EPERM;
    if (!gPresentDiagLock) return ENXIO;
    NVRMPresentStats *snapshot = (NVRMPresentStats *)IOMalloc(sizeof *snapshot);
    if (!snapshot) return ENOMEM;
    IOSimpleLockLock(gPresentDiagLock);
    const unsigned char *src = (const unsigned char *)&gPresentStats; unsigned char *dst = (unsigned char *)snapshot;
    for (unsigned i = 0; i < sizeof *snapshot; ++i) dst[i] = src[i];
    IOSimpleLockUnlock(gPresentDiagLock);
    int ret = SYSCTL_OUT(req, snapshot, sizeof *snapshot); IOFree(snapshot, sizeof *snapshot); return ret;
}
SYSCTL_INT(_debug, OID_AUTO, nvaccel_present_diag, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gPresentDiagEnabled, 0, "PRESENTDIAG-r1: collect timings only, 0 disables");
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_present_stats, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_present_stats_sysctl, "S,NVRMPresentStats", "PRESENTDIAG-r1: coherent read-only timing snapshot");
static bool gSysctlInstalled = false;
static void nvaccelInstallSysctl(void)
{
    if (gSysctlInstalled) return;
#ifdef NMX_ACCEL
    // only this one: the NVIDIA accelerator may be loaded too, and owns the debug.nvaccel* names
    sysctl_register_oid(&sysctl__debug_nmxaccel_headless);
    gSysctlInstalled = true;
#else
    if (!gPresentDiagLock) gPresentDiagLock = IOSimpleLockAlloc();
    if (!gPresentTimingFn) gPresentTimingFn = OSSymbol::withCStringNoCopy(NVRM_PRESENT_TIMING_FN);
    sysctl_register_oid(&sysctl__debug_nvaccel_present_copy_mode);
    if (!gProbeLock) gProbeLock = IOLockAlloc();
    if (!gScanoutWcLock) gScanoutWcLock = IOLockAlloc();
    sysctl_register_oid(&sysctl__debug_nvaccel_scanout_wc);
    sysctl_register_oid(&sysctl__debug_nvaccel_dbflip);
    sysctl_register_oid(&sysctl__debug_nvaccel_stale_checks); sysctl_register_oid(&sysctl__debug_nvaccel_stale_fixed);
    sysctl_register_oid(&sysctl__debug_nvaccel_stale_failed); sysctl_register_oid(&sysctl__debug_nvaccel_stale_valid);
    sysctl_register_oid(&sysctl__debug_nvaccel_dbflip_state);
    sysctl_register_oid(&sysctl__debug_nvaccel_scanout_wc_state);
    sysctl_register_oid(&sysctl__debug_nvaccel_cacheprobe_run);
    sysctl_register_oid(&sysctl__debug_nvaccel_cacheprobe);
    sysctl_register_oid(&sysctl__debug_nvaccel_cacheprobe_split);
    ALOG("CACHEPROBE-r1: probe on demand, copy mode 3 = read/write split; SCANOUTWC-r1 %s", gScanoutWcEnable ? "on" : "off");
    ALOG("PRESENTCOPY-r1: copy mode %d, WC completion fence enabled", gPresentCopyMode);
    sysctl_register_oid(&sysctl__debug_nvaccel_present_diag);
    sysctl_register_oid(&sysctl__debug_nvaccel_present_stats);
    ALOG("PRESENTDIAG-r1: read-only copy/ready/gap/vblank timing available (enabled %d)", gPresentDiagLock ? gPresentDiagEnabled : 0);
    sysctl_register_oid(&sysctl__debug_nvaccelfb);
    sysctl_register_oid(&sysctl__debug_nvaccel_headless);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc);       sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc);     sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_sticky);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_parked);      sysctl_register_oid(&sysctl__debug_nvaccel_vm_unparked);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_park_released);
    sysctl_register_oid(&sysctl__debug_nvaccel_res_new);        sysctl_register_oid(&sysctl__debug_nvaccel_res_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_commit);     sysctl_register_oid(&sysctl__debug_nvaccel_map_release);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok);         sysctl_register_oid(&sysctl__debug_nvaccel_iop_fail);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_empty);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_res);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_surf);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_blank);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_skip);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip);       sysctl_register_oid(&sysctl__debug_nvaccel_iop_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_home);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_lock);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_novram); sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_stale);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_crc_head);       sysctl_register_oid(&sysctl__debug_nvaccel_crc);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_hit);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_miss);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_refused); sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_drop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h0);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h1);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h2);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h3);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h4);      sysctl_register_oid(&sysctl__debug_nvaccel_igpu);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_published); sysctl_register_oid(&sysctl__debug_nvaccel_heads_registered);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_piped);
    gSysctlInstalled = true;
    ALOG("sysctl debug.nvaccelfb installed -- `sudo sysctl -w debug.nvaccelfb=1` registers the framebuffer");
#endif
}

static inline void nvAccelZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static struct NVRMRmHandles gRmHandles;
static bool                 gRmHandlesValid;

static unsigned nvRmHandlesLogged;

static bool nvAccelRmHandles(struct NVRMRmHandles *out)
{
    if (gRmHandlesValid) { *out = gRmHandles; return true; }
    NVAccel *a = NVAccel::gAccel;
    if (!a) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no NVAccel instance yet -- too early"); }
        return false;
    }
    if (!a->fFB) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no framebuffer has published yet -- will retry"); }
        return false;
    }
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_GPUVA_FN_HANDLES);
    if (!sym) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: OSSymbol allocation failed"); }
        return false;
    }
    struct NVRMRmHandles h;
    nvAccelZero(&h, sizeof h);
    h.version = NVRM_GPUVA_ABI_VERSION;
    IOReturn r = a->fFB->callPlatformFunction(sym, false, &h, nullptr, nullptr, nullptr);
    sym->release();
    if (r != kIOReturnSuccess || !h.kapiDevice) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: framebuffer gave no kapi device (ret 0x%x, kapiDevice %p)",
                 (unsigned)r, h.kapiDevice); }
        return false;
    }
    gRmHandles = h; gRmHandlesValid = true;
    ALOG("RM handles: kapiDevice %p (hClient/hDevice resolved inside NVRM by name) "
         "*** the accelerator can now build GPU page tables ***", h.kapiDevice);
    *out = h;
    return true;
}

static IOService *nvAccelNVRM()
{
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nullptr;
    if (!a->fNVRM) {
        OSDictionary *m = IOService::serviceMatching("NVRM");
        if (m) a->fNVRM = IOService::waitForMatchingService(m, 100ULL * 1000 * 1000);
    }
    return a->fNVRM;
}

static IOReturn nvAccelGpuVaCall(const char *fn, struct NVRMGpuVaRequest *g)
{
    IOService *nvrm = nvAccelNVRM();
    if (!nvrm) return kIOReturnNotAttached;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(fn);
    if (!sym) return kIOReturnNoMemory;
    IOReturn r = nvrm->callPlatformFunction(sym, false, g, nullptr, nullptr, nullptr);
    sym->release();
    return r;
}

#define NVRM_SS_KERNEL_ABI 1
#include "../nvrm_surfshare_abi.h"
extern "C" {
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *accel, void *surface, unsigned int plane);
void  _ZN16IOAccelVidMemory4wireEv(void *vidmem);
void  _ZN20IOSurfaceDeviceCache10setCurrentEv(void *cache);
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface);
void  _ZN22IOGraphicsAccelerator29lock_busyEv(void *accel);
void  _ZN22IOGraphicsAccelerator211unlock_busyEv(void *accel);
}
static bool nvFamLock(NVAccel *a, bool tryOnly)
{
    unsigned char *b = (unsigned char *)a;
    IOLock *lk = *(IOLock *volatile *)(b + 0x88);
    if (!lk) return false;
    if (tryOnly) { if (!IOLockTryLock(lk)) return false; }
    else {
        OSIncrementAtomic((volatile SInt32 *)(b + 0x90));
        IOLockLock(lk);
        OSDecrementAtomic((volatile SInt32 *)(b + 0x90));
    }
    _ZN22IOGraphicsAccelerator29lock_busyEv(a);
    a->acceleratorDidLock("", 0);
    return true;
}
static void nvFamUnlock(NVAccel *a)
{
    a->acceleratorWillUnlock("", 0);
    _ZN22IOGraphicsAccelerator211unlock_busyEv(a);
    IOLockUnlock(*(IOLock *volatile *)((unsigned char *)a + 0x88));
}
static unsigned gNVSVLockMiss;
enum { kNVSVPend = 512 };
static unsigned gNVSVPendId[kNVSVPend];
static NVResource *gNVSVPendResource[kNVSVPend];
static IOSimpleLock *gNVSVPendLock;
static bool nvSVPend(unsigned id, int op, NVResource *resource = nullptr)
{
    if (!id) return op == 1 ? false : false;
    if (!gNVSVPendLock) return op == 1 ? false : false;
    bool r = false; int freeSlot = -1;
    IOSimpleLockLock(gNVSVPendLock);
    for (int i = 0; i < kNVSVPend; i++) {
        if (gNVSVPendId[i] == id) {
            r = true;
            if (op == 0) { gNVSVPendId[i] = 0; gNVSVPendResource[i] = nullptr; }
            else if (op == 1) gNVSVPendResource[i] = resource;
            break;
        }
        if (!gNVSVPendId[i] && freeSlot < 0) freeSlot = i;
    }
    if (op == 1 && !r) { if (freeSlot >= 0) { gNVSVPendId[freeSlot] = id; gNVSVPendResource[freeSlot] = resource; r = true; } }
    else if (op == 1) r = true;
    IOSimpleLockUnlock(gNVSVPendLock);
    return r;
}
// A pending mark belongs to the resource that last dirtied the surface. Never
// dereference a device cache during destruction, or clear a newer owner's mark.
static void nvSVForgetResource(NVResource *resource)
{
    if (!resource || !gNVSVPendLock) return;
    IOSimpleLockLock(gNVSVPendLock);
    for (unsigned i = 0; i < kNVSVPend; i++) if (gNVSVPendResource[i] == resource) {
        gNVSVPendId[i] = 0;
        gNVSVPendResource[i] = nullptr;
    }
    IOSimpleLockUnlock(gNVSVPendLock);
}
static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void nvSVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
static unsigned nvAccelSurfDirty(void *cache, NVResource *res, unsigned plane)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[0x14];
    unsigned int fl = *(volatile unsigned int *)(r + 0xc);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return NVRM_SS_NO_CACHE;
    if (!gNVSVPendLock) gNVSVPendLock = IOSimpleLockAlloc();
    if (!nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1, res)) return NVRM_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    return 0;
}
static IOReturn nvAccelSurfVram(struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)
{
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != NVRM_SURFSHARE_VERSION) { p->status = NVRM_SS_BAD_VERSION; return kIOReturnSuccess; }
    NVAccel *a = NVAccel::gAccel;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (!task || !a || !nvAccelRmHandles(&h)) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
        if (!gNVSVRoot) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(gNVSVRoot, p->surfaceID, task);
    if (!surf) { p->status = NVRM_SS_NO_SURFACE; gNVSVRefused++; return kIOReturnSuccess; }
    if (!nvFamLock(a, false)) { ((OSObject *)surf)->release(); p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (cmd == NVRM_ESC_SURF_DIRTY) {
        p->status = (cache && res && vm && vm->fRmHandle) ? nvAccelSurfDirty(cache, res, p->plane) : NVRM_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, p->plane,
                 res ? (unsigned)((volatile unsigned char *)res)[0x14] : 0u, p->status, gNVSVDirty, gNVSVDirtyRefused);
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (vm && !vm->fRmHandle && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) p->status = NVRM_SS_NO_CACHE;
    else if (!vm || !vm->fRmHandle || !vm->fBytes) p->status = NVRM_SS_NO_VRAM;
    else if (p->size > vm->fBytes) p->status = NVRM_SS_TOO_SMALL;
    else {
        struct NVRMSurfShareRm g; nvSVZero(&g, sizeof g);
        g.version = NVRM_SS_RM_VERSION; g.op = NVRM_SS_RM_SHARE_KAPI;
        g.kapiDevice = h.kapiDevice; g.kapiMemory = vm->fRmHandle; g.target = p->hClient; g.status = NVRM_SS_OFF;
        IOService *nvrm = nvAccelNVRM();
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_RM);
        if (nvrm && sym && nvrm->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) == kIOReturnSuccess) {}
        if (sym) sym->release();
        p->status = g.status; p->hSrcClient = g.hClient; p->hSrcMemory = g.hObject; p->size = vm->fBytes;
    }
    if (p->status == 0) gNVSVOk++; else gNVSVRefused++;
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, p->plane, cache, res, vm, vm ? vm->fBytes : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

#include <mach/mach_time.h>
static IOLock *volatile gNVSVStageLock;
static IOBufferMemoryDescriptor *gNVSVStageBuf;
static unsigned gNVSVStaged, gNVSVStageNoBuf, gNVSVStageBackoff;
static IOLock *nvSVStageLock(void)
{
    if (!gNVSVStageLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNVSVStageLock)) IOLockFree(l);
    }
    return gNVSVStageLock;
}
static IOBufferMemoryDescriptor *nvSVStage(unsigned long long len)
{
    if (gNVSVStageBuf && gNVSVStageBuf->getLength() >= len) return gNVSVStageBuf;
    if (len > (64ull << 20) || gNVSVStageBackoff) { if (gNVSVStageBackoff) gNVSVStageBackoff--; return nullptr; }
    const unsigned long long want = (len + 0xFFFFFull) & ~0xFFFFFull;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                      (vm_size_t)want, PAGE_SIZE);
    if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
    if (!b) {
        gNVSVStageNoBuf++; gNVSVStageBackoff = 1024;
        ALOG("surfvram pageoff: no %llu B physically contiguous staging buffer (miss %u) -- per-page copies for the next 1024", want, gNVSVStageNoBuf);
        return nullptr;
    }
    if (gNVSVStageBuf) { gNVSVStageBuf->complete(); gNVSVStageBuf->release(); }
    gNVSVStageBuf = b;
    ALOG("surfvram pageoff: staging buffer %llu B, physically contiguous", want);
    return b;
}

void NVResource::pageoff(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    volatile unsigned char *r = (volatile unsigned char *)this;
    void *cache = *(void *volatile *)(r + 0xe0);
    NVVidMemory *vm = nvres_vidmem(this);
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    if (!surf || !vm || !vm->fRmHandle || vm->fScanout || !vm->fBytes || !nvAccelRmHandles(&h)) {
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        gNVSVPageoffFail++;
        ALOG("surfvram pageoff: res %p surface pages could not be wired (md %p) -- family pageoff instead", this, md);
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    bool mayUnwire = true;
    unsigned long long len = md->getLength();
    if (len > vm->fBytes) len = vm->fBytes;
    unsigned long long pages = (len + 4095) >> 12;
    bool ok = false; unsigned st = NVRM_SS_OFF; unsigned long long usec = 0, cpuUs = 0;
    bool staged = false;
    if (IOLock *lk = nvSVStageLock()) {
        IOLockLock(lk);
        IOBufferMemoryDescriptor *sb = nvSVStage(len);
        IOByteCount sl = 0;
        addr64_t sp = sb ? sb->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) : 0;
        unsigned long long *spa = (sp && sl >= len && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *spa) : nullptr;
        if (spa) {
            for (unsigned long long k = 0; k < pages; k++) spa[k] = sp + k * 4096;
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = spa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec;
            if (st == 0) {
                const uint64_t t0 = mach_absolute_time();
                const IOByteCount w = md->writeBytes(0, sb->getBytesNoCopy(), (IOByteCount)len);
                cpuUs = (mach_absolute_time() - t0) / 1000;
                ok = (w == (IOByteCount)len);
                if (!ok) st = NVRM_SS_TOO_SMALL;
                else gNVSVStaged++;
            } else if (st == 0x65u) {
                ALOG("surfvram pageoff: CE copy TIMED OUT into the staging buffer -- abandoning it (%llu B)", (unsigned long long)sb->getLength());
                gNVSVStageBuf = nullptr;
            }
            staged = true;
            IOFree(spa, pages * sizeof *spa);
        }
        IOLockUnlock(lk);
    }
    unsigned long long *pa = (!staged && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *pa) : nullptr;
    if (pa) {
        unsigned long long i = 0;
        for (IOByteCount off = 0; i < pages; ) {
            IOByteCount seglen = 0;
            addr64_t phys = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
            if (!phys || !seglen) break;
            for (IOByteCount k = 0; k < seglen && i < pages; k += 4096, off += 4096) pa[i++] = phys + k;
        }
        if (i == pages) {
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = pa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec; ok = st == 0;
            if (st == 0x65u) mayUnwire = false;
        }
        IOFree(pa, pages * sizeof *pa);
    }
    if (mayUnwire) md->complete(kIODirectionNone);
    else ALOG("surfvram pageoff: res %p CE copy TIMED OUT -- surface pages stay wired (%llu B)", this, len);
    if (ok) {
        nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0);
        volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
        if (valid) valid[0] |= 1;
        gNVSVPageoff++;
    } else gNVSVPageoffFail++;
    if (out) *out = ok;
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kNvMaxFB], *gIopFlipPrev[kNvMaxFB];
// Failed KAPI calls do not distinguish rejection from unknown completion.
// Retain their candidate surfaces until a later confirmed latch retires them.
static IOSurface *gIopUncertain[kNvMaxFB][8];
static IOSimpleLock *gIopRetireLock;
static volatile SInt64 gCntIopRetireHeld, gCntIopRetireReleased;
NVACCEL_CNT(nvaccel_retire_held, gCntIopRetireHeld, "candidate surfaces retained after uncertain flip completion");
NVACCEL_CNT(nvaccel_retire_released, gCntIopRetireReleased, "uncertain candidates released after confirmed latch");
static bool nvIopRetireRoom(unsigned head) {
    if (!gIopRetireLock) {
        IOSimpleLock *lock = IOSimpleLockAlloc();
        if (lock && !OSCompareAndSwapPtr(nullptr, lock, (void *volatile *)&gIopRetireLock)) IOSimpleLockFree(lock);
    }
    if (!gIopRetireLock || head >= kNvMaxFB) return false;
    bool room = false;
    IOSimpleLockLock(gIopRetireLock);
    for (unsigned i = 0; i < 8; i++) if (!gIopUncertain[head][i]) { room = true; break; }
    IOSimpleLockUnlock(gIopRetireLock);
    return room;
}
// Takes ownership of the retain acquired BEFORE submitting the flip.
static void nvIopRetireUncertain(unsigned head, IOSurface *surface) {
    bool duplicate = false, stored = false;
    IOSimpleLockLock(gIopRetireLock);
    for (unsigned i = 0; i < 8; i++) if (gIopUncertain[head][i] == surface) { duplicate = true; break; }
    if (!duplicate) for (unsigned i = 0; i < 8; i++) if (!gIopUncertain[head][i]) {
        gIopUncertain[head][i] = surface; stored = true; break;
    }
    IOSimpleLockUnlock(gIopRetireLock);
    if (duplicate) ((OSObject *)surface)->release();
    else OSAddAtomic64(1, &gCntIopRetireHeld);
    // Never release an uncertain candidate even if concurrent submissions fill
    // the bounded slots. Admission normally prevents this emergency leak.
    if (!duplicate && !stored) ALOG("retirement: head %u slots exhausted; retaining uncertain surface until reboot", head);
}
static void nvIopRetireConfirmed(unsigned head) {
    if (!gIopRetireLock || head >= kNvMaxFB) return;
    IOSurface *retired[8] = {};
    IOSimpleLockLock(gIopRetireLock);
    for (unsigned i = 0; i < 8; i++) { retired[i] = gIopUncertain[head][i]; gIopUncertain[head][i] = nullptr; }
    IOSimpleLockUnlock(gIopRetireLock);
    for (unsigned i = 0; i < 8; i++) if (retired[i]) {
        ((OSObject *)retired[i])->release(); OSAddAtomic64(1, &gCntIopRetireReleased);
    }
}
static void nvIopPromoteConfirmed(unsigned head, IOSurface *surface) {
    IOSurface *old = gIopFlipFront[head], *prev = gIopFlipPrev[head];
    gIopFlipFront[head] = surface; gIopFlipPrev[head] = nullptr;
    gIopFlipped[head] = true; nvDbSetFront(head, 2);
    // Success from nvFlipToSurfacePure means getFlipPendingStatus confirmed
    // completion. The old front no longer needs an extra frame of retention.
    if (old) ((OSObject *)old)->release();
    if (prev) ((OSObject *)prev)->release();
    nvIopRetireConfirmed(head);
}
static unsigned gIopFlipLogs;
static bool nvAccelIopFlipNo(int why) { OSAddAtomic64(1, &gCntIopFlipNo[why & 7]); return false; }
struct NVIopFlipCache { IOSurface *s; NVResource *res; NVVidMemory *vm; void *kmem; unsigned long long bytes; unsigned long long use; };
// Four compositor buffers cover common double/triple buffering without pinning eight
// full-screen allocations in the small BAR1 window. Active scanout has its own retain.
static constexpr unsigned kIopFlipCacheSlots = 4;
static NVIopFlipCache gIopFlipCache[kIopFlipCacheSlots];
static IOSimpleLock *gIopFlipCacheLock;
static unsigned long long gIopFlipCacheTick;
static bool nvAccelIopCacheGet(IOSurface *s, NVResource **res, NVVidMemory **vm, void **kmem, unsigned long long *bytes)
{
    if (!gIopFlipCacheLock) return false;
    bool hit = false;
    IOSimpleLockLock(gIopFlipCacheLock);
    for (unsigned i = 0; i < kIopFlipCacheSlots; i++) if (gIopFlipCache[i].s == s) {
        NVVidMemory *v = gIopFlipCache[i].vm;
        if (v && v->fRmHandle == gIopFlipCache[i].kmem && v->fBytes == gIopFlipCache[i].bytes) {
            *res = gIopFlipCache[i].res; *vm = v; *kmem = gIopFlipCache[i].kmem; *bytes = gIopFlipCache[i].bytes;
            gIopFlipCache[i].use = ++gIopFlipCacheTick; hit = true;
        }
        break;
    }
    IOSimpleLockUnlock(gIopFlipCacheLock);
    return hit;
}
static void nvAccelIopCachePut(IOSurface *s, NVResource *res, NVVidMemory *vm, void *kmem, unsigned long long bytes)
{
    if (!gIopFlipCacheLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopFlipCacheLock)) IOSimpleLockFree(l); }
    if (!gIopFlipCacheLock) return;
    IOSurface *drop = nullptr;
    ((OSObject *)s)->retain();
    IOSimpleLockLock(gIopFlipCacheLock);
    unsigned k = 0;
    for (unsigned i = 0; i < kIopFlipCacheSlots; i++) {
        if (gIopFlipCache[i].s == s) { k = i; break; }
        if (!gIopFlipCache[i].s) { k = i; break; }
        if (gIopFlipCache[i].use < gIopFlipCache[k].use) k = i;
    }
    drop = gIopFlipCache[k].s;
    gIopFlipCache[k].s = s; gIopFlipCache[k].res = res; gIopFlipCache[k].vm = vm; gIopFlipCache[k].kmem = kmem;
    gIopFlipCache[k].bytes = bytes; gIopFlipCache[k].use = ++gIopFlipCacheTick;
    IOSimpleLockUnlock(gIopFlipCacheLock);
    if (drop) ((OSObject *)drop)->release();
}
extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}
struct NVIopAsync { void *pipe; IOService *fb; IOSurface *s; struct NVRMFlipRequest f; volatile SInt32 busy; struct thread_call *call; };
static NVIopAsync gIopAs[kNvMaxFB];
static SInt32 gIopAsyncRefuseRun;
static bool nvDbBusy(const void *pipe);
static bool nvAccelIopAsyncBusy(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gIopAs[h].pipe == pipe && __atomic_load_n(&gIopAs[h].busy, __ATOMIC_ACQUIRE)) return true;
    return nvDbBusy(pipe);
}
static void nvAccelIopAsyncRun(void *p0, void *)
{
    NVIopAsync *e = (NVIopAsync *)p0;
    const unsigned head = (unsigned)(e - gIopAs);
    IOSurface *s = e->s; e->s = nullptr;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? e->fb->callPlatformFunction(sym, false, &e->f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr == kIOReturnSuccess) {
        gIopAsyncRefuseRun = 0;
        nvIopPromoteConfirmed(head, s);
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: ASYNC swap #%lld head %u: surface 0x%x %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, s->getSurfaceID(), e->f.width, e->f.height, e->f.pitch, e->f.flipResult);
    } else {
        OSAddAtomic64(1, &gCntIopAsyncRefused); OSAddAtomic64(1, &gCntIopFlipNo[7]);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: async flip REFUSED (0x%x, flipResult %d) head %u -- that frame is not on the panel",
                                                     (unsigned)rr, e->f.flipResult, head); }
        if (++gIopAsyncRefuseRun >= 3 && gIopAsync) {
            gIopAsync = 0;
            ALOG("iop: b89k 3 async refusals in a row -- debug.nvaccel_iop_async=0 (synchronous flip, which copies on refusal)");
        }
        nvIopRetireUncertain(head, s);
    }
    void *pipe = e->pipe; IOService *fb = e->fb;
    __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(pipe, nullptr);
    fb->release(); ((OSObject *)pipe)->release();
}
static bool nvAccelIopAsyncStart(void *pipe, IOService *fb, unsigned head, IOSurface *s, const struct NVRMFlipRequest *f)
{
    NVIopAsync *e = &gIopAs[head];
    for (unsigned i = 0; i < 50 && __atomic_load_n(&e->busy, __ATOMIC_ACQUIRE); i++) IOSleep(1);
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        OSAddAtomic64(1, &gCntIopAsyncDrop);
        return true;
    }
    if (!e->call) e->call = thread_call_allocate(nvAccelIopAsyncRun, e);
    if (!e->call) return false;
    ((OSObject *)s)->retain();
    ((OSObject *)pipe)->retain(); fb->retain();
    e->pipe = pipe; e->fb = fb; e->s = s;
    for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->f)[i] = ((const unsigned char *)f)[i];
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    thread_call_enter(e->call);
    return true;
}
// ---- DBFLIP-r1 -------------------------------------------------------------------------------------------------
// Why: NVKMS 610 delivers kernel vblank callbacks only through RG-semaphore interrupts, which its Turing HAL (nvEvoC5)
// does not implement (SetupVBlankRgSemaphoreInterrupt == NULL), so registerVblankIntrCallback is refused on this
// RTX 2070 SUPER and NVRMFB's VBL is a timer; copying into the visible scanout then tears. A flip, however, is latched
// by the display engine at the real vblank on every GPU. nvFlipToSurfacePure blocks until NVKMS reports the flip done
// (getFlipPendingStatus), so a completed flip proves the old buffer is no longer scanned out.
// Safety: a refused/faulted flip never leaves the panel frozen. After 3 refusals in a row this head stops flipping and
// copies into whichever buffer is on the panel (today's behaviour). The back buffer is only freed after the panel is
// proven home (a modeset, or an accepted home flip).
struct NVDbHead {
    void *kmem; volatile void *kva; unsigned long long cookie, bytes;
    unsigned w, h, pitch; unsigned long long homePhys, epoch; bool epochOk;
    int front;               // 0 = home, 1 = back buffer, 2 = compositor zero-copy surface
    bool off;                // this head stopped flipping (refusals or allocation failure) until its geometry changes
    unsigned refusedRun, homeFails;
    volatile SInt32 busy; void *pipe; IOService *fb; int target; struct NVRMFlipRequest f; struct thread_call *call;
};
static NVDbHead gDb[kNvMaxFB];
static int gDbFlip = 1;
static volatile SInt64 gCntDbFlips, gCntDbRefused, gCntDbDrops, gCntDbAllocs, gCntDbAllocFail, gCntDbHomes, gCntDbCopiesFront, gCntDbAsync;
static SInt32 gDbLogs;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_dbflip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gDbFlip, 0,
           "DBFLIP-r1: 1 copy into the hidden buffer and flip at vblank (default), 0 copy into the visible buffer (homes the panel first)");
static bool nvDbBusy(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gDb[h].pipe == pipe && __atomic_load_n(&gDb[h].busy, __ATOMIC_ACQUIRE)) return true;
    return false;
}
static void nvDbRelease(IOService *fb, NVDbHead *st)
{
    if (!st->kmem) return;
    struct NVRMVramRequest r; nvSVZero(&r, sizeof r);
    r.version = NVRM_VRAM_ABI_VERSION; r.handle = st->kmem; r.kva = (void *)st->kva; r.actualSize = st->bytes; r.allocationCookie = st->cookie;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_FREE);
    IOReturn rr = (sym && fb) ? fb->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    ALOG("DBFLIP-r1: back buffer %p released (0x%x)", st->kmem, (unsigned)rr);
    st->kmem = nullptr; st->kva = nullptr; st->cookie = 0; st->bytes = 0;
}
static bool nvDbAlloc(IOService *fb, NVDbHead *st, const struct NVRMVramRequest *q)
{
    struct NVRMVramRequest r; nvSVZero(&r, sizeof r);
    r.version = NVRM_VRAM_ABI_VERSION; r.size = (unsigned long long)q->pitch * q->height;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_ALLOC);
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr != kIOReturnSuccess || !r.handle || !r.kva || r.actualSize < (unsigned long long)q->pitch * q->height) {
        OSAddAtomic64(1, &gCntDbAllocFail);
        ALOG("DBFLIP-r1: back buffer %ux%u pitch %u REFUSED (0x%x) -- this head keeps copying into the visible buffer", q->width, q->height, q->pitch, (unsigned)rr);
        return false;
    }
    st->kmem = r.handle; st->kva = (volatile void *)r.kva; st->cookie = r.allocationCookie; st->bytes = r.actualSize;
    // vramGrant already zeroed the allocation. performTransaction overwrites every
    // visible pixel before the first flip; reading the old WC scanout here is redundant.
    OSAddAtomic64(1, &gCntDbAllocs);
    ALOG("DBFLIP-r1: back buffer %llu bytes VRAM phys 0x%llx for %ux%u pitch %u -- double buffering on", st->bytes, r.phys, q->width, q->height, q->pitch);
    return true;
}
static void nvDbFinish(NVDbHead *st, int target, bool ok, int flipResult)
{
    if (ok) {
        nvDbSetFront((unsigned)(st - gDb), target); st->refusedRun = 0;
        if (target == 0) { st->homeFails = 0; OSAddAtomic64(1, &gCntDbHomes); }
        const SInt64 n = OSAddAtomic64(1, &gCntDbFlips);
        if (n < 3 || n == 1000) ALOG("DBFLIP-r1: flip #%lld to %s latched (flipResult %d)", (long long)n + 1, target ? "back" : "home", flipResult);
        return;
    }
    OSAddAtomic64(1, &gCntDbRefused);
    if (target == 0) st->homeFails++;
    if (gDbLogs < 8) { OSIncrementAtomic(&gDbLogs); ALOG("DBFLIP-r1: flip to %s REFUSED (flipResult %d), panel stays on %s", target ? "back" : "home", flipResult, st->front ? "back" : "home"); }
    if (++st->refusedRun >= 3 && !st->off) { st->off = true; ALOG("DBFLIP-r1: 3 refusals in a row -- this head copies into the visible buffer from now on"); }
}
static IOReturn nvDbFlipNow(NVDbHead *st, IOService *fb, struct NVRMFlipRequest *f)
{
    const uint64_t t0 = nvPresentNowNs();
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    const uint64_t ms = (nvPresentNowNs() - t0) / 1000000;
    // A pure flip waits at most one refresh. Far longer means NVRMFB did a full modeset (home flip on an unlit head)
    // or waited on a stuck completion: alternating would then flicker or stall, so this head stops flipping.
    if (ms > 60 && !st->off) { st->off = true; ALOG("DBFLIP-r1: a flip took %llu ms -- this head copies into the visible buffer from now on", ms); }
    return rr;
}
static void nvDbAsyncRun(void *p0, void *)
{
    NVDbHead *st = (NVDbHead *)p0;
    IOReturn rr = nvDbFlipNow(st, st->fb, &st->f);
    nvDbFinish(st, st->target, rr == kIOReturnSuccess, st->f.flipResult);
    OSAddAtomic64(1, &gCntDbAsync);
    void *pipe = st->pipe; IOService *fb = st->fb;
    __atomic_store_n(&st->busy, 0, __ATOMIC_RELEASE);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(pipe, nullptr); // the transaction completes at the latch
    fb->release(); ((OSObject *)pipe)->release();
}
static volatile void *nvDbPrepare(IOService *fb, unsigned head, const struct NVRMVramRequest *q, int *target)
{
    *target = -1;
    if (!fb || head >= kNvMaxFB || !fb->metaCast("NVRMNVDAFramebuffer") || (gNvHeadsPiped & (gNvHeadsPiped - 1))) return q->kva;
    NVDbHead *st = &gDb[head];
    // The buffer the previous flip moved away from may be scanned out until that flip latches.
    for (unsigned i = 0; i < 50 && __atomic_load_n(&st->busy, __ATOMIC_ACQUIRE); i++) IOSleep(1);
    if (__atomic_load_n(&st->busy, __ATOMIC_ACQUIRE)) { OSAddAtomic64(1, &gCntDbDrops); return nullptr; }
    NVRMPresentTiming t; nvSVZero(&t, sizeof t);
    if (nvPresentTiming(fb, &t)) {
        if (st->epochOk && t.modeEpoch != st->epoch) nvDbSetFront(head, 0); // a modeset scans the home surface out again
        st->epoch = t.modeEpoch; st->epochOk = true;
    }
    if (st->w != q->width || st->h != q->height || st->pitch != q->pitch || st->homePhys != q->phys) {
        // Geometry only changes through a modeset, which put the home surface back on the panel.
        nvDbSetFront(head, 0);
        nvDbRelease(fb, st);
        st->w = q->width; st->h = q->height; st->pitch = q->pitch; st->homePhys = q->phys;
        st->off = false; st->refusedRun = 0; st->homeFails = 0;
    }
    if (!gDbFlip || st->off) {
        // A failed transition can leave the compositor surface visible. Keep retrying
        // a home flip after copying; never pretend a write to home updated that surface.
        if (st->front == 2) { *target = 0; return q->kva; }
        if (st->front == 1) {
            // Bring the panel home through a flip; if home flips keep failing, keep the visible back buffer live.
            if (st->homeFails < 2) { *target = 0; return q->kva; }
            OSAddAtomic64(1, &gCntDbCopiesFront); return st->kva;
        }
        if (st->kmem && !gDbFlip) nvDbRelease(fb, st); // the panel is home: the back buffer is idle
        return q->kva;
    }
    if (!st->kmem && !nvDbAlloc(fb, st, q)) { st->off = true; return q->kva; }
    *target = (st->front == 1) ? 0 : 1; // from home or a zero-copy surface, the back buffer is free
    return *target ? st->kva : q->kva;
}
static void nvDbSubmit(void *pipe, IOService *fb, unsigned head, int target)
{
    NVDbHead *st = &gDb[head];
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION;
    if (target) { f.kapiMemory = st->kmem; f.allocationCookie = st->cookie; f.width = st->w; f.height = st->h; f.pitch = st->pitch; }
    else f.flags = 1u; // home: NVRMFB flips back to its own boot surface
    if (gIopAsync && pipe) {
        if (!st->call) st->call = thread_call_allocate(nvDbAsyncRun, st);
        if (st->call) {
            ((OSObject *)pipe)->retain(); fb->retain();
            st->pipe = pipe; st->fb = fb; st->target = target;
            for (unsigned i = 0; i < sizeof f; i++) ((volatile unsigned char *)&st->f)[i] = ((const unsigned char *)&f)[i];
            __atomic_store_n(&st->busy, 1, __ATOMIC_RELEASE);
            thread_call_enter(st->call);
            return;
        }
    }
    IOReturn rr = nvDbFlipNow(st, fb, &f);
    nvDbFinish(st, target, rr == kIOReturnSuccess, f.flipResult);
}
static bool nvDbActive(unsigned head)
{
    return head < kNvMaxFB && gDbFlip && !gDb[head].off && gDb[head].kmem;
}
static void nvDbSetFront(unsigned head, int front)
{
    if (head >= kNvMaxFB) return;
    gDb[head].front = front;
    gIopFlipped[head] = front == 2;
    if (front != 2) {
        // Called only after a confirmed latch/modeset. Release scanout ownership,
        // otherwise old compositor surfaces keep scarce BAR1 mappings alive forever.
        IOSurface *old = gIopFlipFront[head], *prev = gIopFlipPrev[head];
        gIopFlipFront[head] = gIopFlipPrev[head] = nullptr;
        if (old) ((OSObject *)old)->release();
        if (prev) ((OSObject *)prev)->release();
        nvIopRetireConfirmed(head);
    }
}
// ZEROCOPY-r1: NVRM's auto-go only turns zero-copy on with a large BAR1 ("BAR1 not placed" otherwise, because of black
// screens reported with small BARs and the copy path). On this RTX 2070 SUPER (256 MB BAR1, PAT fixed, shared IOSurface
// VRAM, DBFLIP) a live test flipped 1004 compositor surfaces with 0 refusals and a changing display CRC. It is armed
// 60 s after the first copied frame -- the same settle NVRM uses -- unless -nvrmnoflip is set or someone already wrote
// debug.nvaccel_iop_flip. nvAccelIopFlip still copies whenever the surface's VRAM is not the newest copy.
static uint64_t gZeroCopyFirstNs; static bool gZeroCopyArmed;
static void nvZeroCopyArm(void)
{
    if (gZeroCopyArmed) return;
    const uint64_t now = nvPresentNowNs();
    if (!gZeroCopyFirstNs) { gZeroCopyFirstNs = now; return; }
    if (now - gZeroCopyFirstNs < 60000000000ull) return;
    gZeroCopyArmed = true;
    char nb[8];
    if (PE_parse_boot_argn("-nvrmnoflip", nb, sizeof nb)) { ALOG("ZEROCOPY-r1: -nvrmnoflip set -- zero-copy stays off"); return; }
    if (!OSCompareAndSwap((UInt32)-1, 1, (volatile UInt32 *)&gIopFlip)) return;
    ALOG("ZEROCOPY-r1: zero-copy scan-out of the compositor's surfaces armed (debug.nvaccel_iop_flip=1)");
}
static int nvaccel_dbflip_state_sysctl SYSCTL_HANDLER_ARGS {
    if (req->newptr) return EPERM;
    char t[384];
    const NVDbHead *st = &gDb[0];
    snprintf(t, sizeof t, "head0 %s front %s back %s %ux%u pitch %u | flips %lld refused %lld dropped %lld homes %lld async %lld allocs %lld allocfail %lld front-copies %lld",
             !gDbFlip ? "OFF(sysctl)" : st->off ? "OFF(refused)" : "ON", st->front == 2 ? "zero-copy" : st->front ? "back" : "home", st->kmem ? "allocated" : "none",
             st->w, st->h, st->pitch, (long long)gCntDbFlips, (long long)gCntDbRefused, (long long)gCntDbDrops, (long long)gCntDbHomes,
             (long long)gCntDbAsync, (long long)gCntDbAllocs, (long long)gCntDbAllocFail, (long long)gCntDbCopiesFront);
    return SYSCTL_OUT(req, t, strlen(t) + 1);
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_dbflip_state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvaccel_dbflip_state_sysctl, "A", "DBFLIP-r1: state and counters");
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (__atomic_load_n(&gIopFlip, __ATOMIC_ACQUIRE) <= 0 || !fb || !s || head >= kNvMaxFB) return false;
    // zero-copy scan-out is an NVIDIA display-engine flip; another vendor's panel (NMIntelFB) always takes the copy
    if (!fb->metaCast("NVRMNVDAFramebuffer")) return false;
    if (!nvIopRetireRoom(head)) return nvAccelIopFlipNo(7);
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nvAccelIopFlipNo(1);
    if (s->getPixelFormat() != 0x42475241u) return nvAccelIopFlipNo(2);
    NVResource *res = nullptr; NVVidMemory *vm = nullptr; void *kmem = nullptr; unsigned long long bytes = 0;
    if (nvAccelIopCacheGet(s, &res, &vm, &kmem, &bytes)) {
        OSAddAtomic64(1, &gCntIopFlipHit);
    } else {
        if (!nvFamLock(a, true)) return nvAccelIopFlipNo(3);
        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
        OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
        res = OSDynamicCast(NVResource, ctx);
        vm = res ? nvres_vidmem(res) : nullptr;
        kmem = (vm && vm->fRmHandle && vm->fBytes && !vm->fScanout) ? vm->fRmHandle : nullptr;
        bytes = vm ? vm->fBytes : 0;
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        OSAddAtomic64(1, &gCntIopFlipMiss);
        if (kmem) nvAccelIopCachePut(s, res, vm, kmem, bytes);
    }
    volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
    const bool vramNewest = valid && (valid[6] & 1u);
    if (!kmem) return nvAccelIopFlipNo(4);
    if (!vramNewest) return nvAccelIopFlipNo(5);
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || H * P > bytes) return nvAccelIopFlipNo(6);
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.kapiMemory = kmem; f.allocationCookie = vm->fAllocationCookie; f.width = (unsigned)W; f.height = (unsigned)H; f.pitch = (unsigned)P;
    if (gIopAsync && pipe) return nvAccelIopAsyncStart(pipe, fb, head, s, &f);
    ((OSObject *)s)->retain();
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (rr != kIOReturnSuccess) {
        nvIopRetireUncertain(head, s);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: flip REFUSED (0x%x, flipResult %d) head %u %llux%llu pitch %llu -- copying",
                                                     (unsigned)rr, f.flipResult, head, W, H, P); }
        return nvAccelIopFlipNo(7);
    }
    nvIopPromoteConfirmed(head, s);
    const SInt64 n = OSAddAtomic64(1, &gCntIopFlip);
    if (n < 4 || n == 600 || (n & 0xffff) == 0)
        ALOG("iop: zero-copy swap #%lld head %u: surface 0x%x %llux%llu pitch %llu scanned out of its own VRAM (flipResult %d)",
             (long long)n + 1, head, s->getSurfaceID(), W, H, P, f.flipResult);
    return true;
}
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS
{
    unsigned v = 0;
    NVAccel *a = NVAccel::gAccel;
    IOService *fb = a ? a->fFB : nullptr;
    IOService *nub = fb ? fb->getProvider() : nullptr;
    OSNumber *n = nub ? OSDynamicCast(OSNumber, nub->getProperty("nvkms-kapi")) : nullptr;
    struct NvKmsKapiFunctionsTable *k = n ? (struct NvKmsKapiFunctionsTable *)(uintptr_t)n->unsigned64BitValue() : nullptr;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (k && k->getCRC32 && gCrcHead >= 0 && gCrcHead < 8 && nvAccelRmHandles(&h) && h.kapiDevice) {
        struct NvKmsKapiCrcs c; nvSVZero(&c, sizeof c);
        if (k->getCRC32((struct NvKmsKapiDevice *)h.kapiDevice, (NvU32)gCrcHead, &c))
            v = c.compositorCrc32.supported ? c.compositorCrc32.value : (c.outputCrc32.supported ? c.outputCrc32.value : 0);
    }
    int out = (int)v;
    return sysctl_handle_int(oidp, &out, 0, req);
}
static bool nvAccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kNvMaxFB || !gIopFlipped[head] || !fb->metaCast("NVRMNVDAFramebuffer")) return true;
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.flags = 1u;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    if (rr != kIOReturnSuccess && gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: HOME flip refused (0x%x) head %u", (unsigned)rr, head); }
    if (rr == kIOReturnSuccess) nvDbSetFront(head, 0);
    return rr == kIOReturnSuccess;
}
static void nvAccelSurfScanoutSync(IOSurface *s)
{
    if (!s || !NVAccel::gAccel || !nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 2)) return;
    NVAccel *a = NVAccel::gAccel;
    if (!nvFamLock(a, true)) {
        if (++gNVSVLockMiss <= 8 || (gNVSVLockMiss & 1023) == 0)
            ALOG("surfvram scanout: accelerator lock busy, surface %u stays pending (misses %u)",
                 _ZNK9IOSurface12getSurfaceIDEv((void *)s), gNVSVLockMiss);
        return;
    }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    bool out = false;
    if (res) res->pageoff(nullptr, true, &out, 0);
    if (!out) nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 0);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
}
// True when the surface's system memory holds its newest pixels, after moving them there if the GPU drew them into VRAM.
// Unlike nvAccelSurfScanoutSync it does not clear the pending mark when the page-off fails. gtx1650: try-lock only (no
// IOSleep on the copy path) and never hold a frame back; a surface that has not synced yet is copied as it is.
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head)
{
    NVAccel *a = NVAccel::gAccel;
    if (!s || !a) return true;
    const unsigned id = _ZNK9IOSurface12getSurfaceIDEv((void *)s);
    if (nvSVPend(id, 2)) {
        if (nvFamLock(a, true)) {
            void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
            OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
            NVResource *res = OSDynamicCast(NVResource, ctx);
            volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
            if (!res || !valid || (valid[0] & 1u)) {
                nvSVPend(id, 0);
            } else {
                bool out = false;
                res->pageoff(nullptr, true, &out, 0);
                if (out || (valid[0] & 1u)) nvSVPend(id, 0);
            }
            if (cache) ((OSObject *)cache)->release();
            nvFamUnlock(a);
        }
    }
    return true;
}

// ---- STALEGUARD-r1 -----------------------------------------------------------------------------------------------
// With shared IOSurface VRAM (NVMTL_NO_SURFACE_SHARE unset) the GPU draws into VRAM and the system-memory copy the
// display pipe reads is refreshed by a page-off when the surface is marked pending. On 14/10 02:12 the copy path put
// one-colour (black) frames on the panel after a mode change: stale system memory read as the frame. Whatever loses the
// pending mark, the resource's own validity bits still say whether system memory holds the newest pixels. They are
// only consulted for frames that look uniform, so a normal desktop pays nothing; a stale one is paged off first, and if
// that fails the previous picture stays (at most 4 frames in a row, as nvAccelSurfCopyReady does).
static SInt64 gCntStaleChecks, gCntStaleFixed, gCntStaleFailed, gCntStaleValid;
static unsigned gStaleHeld[kNvMaxFB];
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_stale_checks, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntStaleChecks, "STALEGUARD-r1: uniform-looking frames checked");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_stale_fixed, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntStaleFixed, "STALEGUARD-r1: stale system memory paged off before the copy");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_stale_failed, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntStaleFailed, "STALEGUARD-r1: stale and the page-off failed (frame held or shown stale)");
SYSCTL_QUAD(_debug, OID_AUTO, nvaccel_stale_valid, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntStaleValid, "STALEGUARD-r1: uniform frames whose system memory was already current");
static bool nvStaleLooksUniform(const volatile unsigned int *src, unsigned w, unsigned h, unsigned spr)
{
    if (!src || !w || !h || !spr) return false;
    const unsigned first = src[0] & 0xffffffu;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        unsigned sx = (w * (2 * j + 1)) / 16; if (sx >= spr) sx = spr - 1;
        if ((src[(unsigned long)((h * (2 * i + 1)) / 16) * spr + sx] & 0xffffffu) != first) return false;
    }
    return true;
}
static bool nvAccelSurfForceFresh(IOSurface *s, unsigned head)
{
    NVAccel *a = NVAccel::gAccel;
    if (!s || !a) return true;
    OSAddAtomic64(1, &gCntStaleChecks);
    bool locked = false;
    for (unsigned i = 0; i < 4 && !(locked = nvFamLock(a, true)); i++) IOSleep(1);
    if (!locked) return true; // cannot check: same as before this guard
    bool fresh = true, fixed = false;
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
    if (res && valid && !(valid[0] & 1u)) {
        bool out = false; res->pageoff(nullptr, true, &out, 0);
        fresh = out || (valid[0] & 1u); fixed = fresh;
        if (fresh) nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 0);
    } else OSAddAtomic64(1, &gCntStaleValid);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    if (fixed) { const SInt64 n = OSAddAtomic64(1, &gCntStaleFixed); if (n < 4 || (n & 1023) == 0) ALOG("STALEGUARD-r1: surface %u: system memory was stale, paged off before the copy (#%lld)", _ZNK9IOSurface12getSurfaceIDEv((void *)s), (long long)n + 1); }
    if (fresh) { if (head < kNvMaxFB) gStaleHeld[head] = 0; return true; }
    const SInt64 n = OSAddAtomic64(1, &gCntStaleFailed);
    if (n < 8 || (n & 1023) == 0) ALOG("STALEGUARD-r1: surface %u: system memory stale and the page-off failed (#%lld)", _ZNK9IOSurface12getSurfaceIDEv((void *)s), (long long)n + 1);
    if (head < kNvMaxFB && ++gStaleHeld[head] <= 4) return false; // keep the previous picture
    if (head < kNvMaxFB) gStaleHeld[head] = 0;
    return true;
}

static unsigned nvVaAllocLogged, nvVaMapLogged;

static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                            unsigned long long len, unsigned long long align)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !len) return 0;
    if (mm->fGpuVa) return mm->fGpuVa;

    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return 0;

    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.size = len; g.align = align;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_ALLOC, &g) != kIOReturnSuccess || !g.gpuva) {
        if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
            ALOG("gpuVaAlloc: RM refused %llu bytes (align %llu) -> status 0x%x", len, align, g.status); }
        return 0;
    }
    mm->fHVirt = g.hVirt; mm->fGpuVa = g.gpuva; mm->fVaLen = len;
    if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
        ALOG("gpuVaAlloc: %llu bytes -> gpuVA 0x%llx (hVirt 0x%x)", len, g.gpuva, g.hVirt); }
    return g.gpuva;
}

static void nvAccelGpuVaFree(IOAccelMemoryMap *map)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !mm->fHVirt) return;
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = mm->fHVirt; g.hMemory = mm->fMapped ? mm->fHRmMemory : 0; g.size = mm->fVaLen;
    g.gpuva = mm->fGpuVa;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    OSAddAtomic64(1, &gCntMapFree);
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
}

bool NVMemoryMap::commitIntoGPUPageTable()
{
    if (fMapped) return true;
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, fMem);
    void *kapiMem = vm ? vm->fRmHandle : nullptr;
    unsigned long long len = fVaLen ? fVaLen : getLength();
    if (!fHVirt || !kapiMem || !len) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: NOT MAPPED -- hVirt 0x%x kapiMem %p len %llu (mem %p%s)",
                 fHVirt, kapiMem, len, fMem, vm ? "" : ", not an NVVidMemory"); }
        return false;
    }
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return false;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice; g.kapiMemory = kapiMem;
    g.hVirt = fHVirt; g.size = len;
    g.isVidmem = 1;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_MAP, &g) != kIOReturnSuccess) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: MAP_MEMORY_DMA refused gpuVA 0x%llx len %llu -> status 0x%x",
                 fGpuVa, len, g.status); }
        return false;
    }
    fHRmMemory = g.hMemory; fMapped = true;
    OSAddAtomic64(1, &gCntMapCommit);
    if (g.gpuva) fGpuVa = g.gpuva;
    if (fCommits < 8) { fCommits++;
        ALOG("MM commit: MAPPED gpuVA 0x%llx len %llu <- hMem 0x%x  *** the GPU can reach these bytes ***",
             fGpuVa, len, g.hMemory); }
    return true;
}

void NVMemoryMap::releaseFromGPUPageTable()
{
    if (!fMapped || !fHVirt) return;
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = fHVirt; g.hMemory = fHRmMemory; g.size = fVaLen;
    g.gpuva = fGpuVa;
    g.flags = NVRM_GPUVA_FLAG_UNMAP_ONLY;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    fMapped = false;
    OSAddAtomic64(1, &gCntMapRelease);
}

static void nvAccelWriteStamp(int index, unsigned int value)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fStamps || index < 0 || index >= 4096) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (!base) return;
    base[index] = value;
}

static unsigned nvFrontSamples, nvFrontFlips;
static unsigned nvFrontLastSum;
static void nvAccelSampleFront(NVVidMemory *vm, unsigned w, unsigned h, unsigned pitch)
{
    if (!vm || !vm->fKva || !pitch || !w || !h) return;
#define NVFRONT_EVERY 10u
    if (++nvFrontFlips % NVFRONT_EVERY) return;
    if (nvFrontSamples >= 40) return;
    nvFrontSamples++;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    const unsigned p1 = (w > 100  && h > 100)  ? px[100  * ppr + 100]  : 0u;
    const unsigned p2 = (w > 960  && h > 540)  ? px[540  * ppr + 960]  : 0u;
    const unsigned p3 = (w > 1800 && h > 1000) ? px[1000 * ppr + 1800] : 0u;
    const bool moved = (nvFrontSamples > 1) && (sum != nvFrontLastSum);
    ALOG("FRONT SURFACE sample %u: sum 0x%08x %s nonzero %u/%u "
         "px(100,100)=0x%08x px(960,540)=0x%08x px(1800,1000)=0x%08x %ux%u pitch %u",
         nvFrontSamples, sum,
         moved ? "*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***"
               : (nvFrontSamples > 1 ? "(UNCHANGED since last sample -- frozen, not proof of a picture)"
                                     : "(first sample -- motion is the proof, wait for the next)"),
         nonzero, n, p1, p2, p3, w, h, pitch);
    nvFrontLastSum = sum;
}

static unsigned nvSwapProbes;
static void nvAccelResStats(IOAccelResource2 *res, const char *tag)
{
    if (!res) { ALOG("  swap %s: (null)", tag); return; }
    NVVidMemory *vm = nvres_vidmem(res);
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!vm || !vm->fKva || !pitch || !w || !h) {
        ALOG("  swap %s: %ux%u pitch %u vm %p kva %p scanout %d -- NOT SAMPLEABLE",
             tag, w, h, pitch, vm, vm ? vm->fKva : nullptr, vm ? (int)vm->fScanout : -1);
        return;
    }
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    ALOG("  swap %s: %ux%u pitch %u scanout %d  sum 0x%08x  nonzero %u/%u  %s",
         tag, w, h, pitch, (int)vm->fScanout, sum, nonzero, n,
         nonzero ? "*** HAS CONTENT -- THIS IS THE SOURCE ***" : "(empty)");
}
bool nvAccelResHasContent(IOAccelResource2 *res)
{
    if (!res) return false;
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) return false;
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || pitch < 4) return false;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    for (unsigned iy = 1; iy <= 4; iy++)
        for (unsigned ix = 1; ix <= 4; ix++) {
            const unsigned x = (w * ix) / 5, y = (h * iy) / 5;
            if (x < w && y < h && px[y * ppr + x]) return true;
        }
    return false;
}

void nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (nvSwapProbes >= 6) return;
    nvSwapProbes++;
    ALOG("SWAP PROBE %u -- which argument holds the composited frame?", nvSwapProbes);
    nvAccelResStats(a1, "a1");
    nvAccelResStats(a2, "a2");
}

static unsigned nvFlipWhy = 0;
#define NVFLIPWHY(...) do { if (nvFlipWhy < 20) { nvFlipWhy++; ALOG(__VA_ARGS__); } } while (0)

#define NVSCANOUT_COPY_MAX 120
static unsigned nvScanoutCopies = 0;
static bool nvAccelCopyToScanout(IOAccelResource2 *res)
{
    if (nvScanoutCopies >= NVSCANOUT_COPY_MAX) return false;
    if (!res) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("copyToScanout: no accelerator/framebuffer"); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("copyToScanout: refused -- heads piped 0x%x, no way to tell whose frame this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) { NVFLIPWHY("copyToScanout: source has no kva"); return false; }

    const unsigned char *rr = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(rr + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(rr + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(rr + 0xb8);
    if (!w || !h || pitch < 4) { NVFLIPWHY("copyToScanout: source geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q);
    if (ret != kIOReturnSuccess || !q.kva || !q.pitch) {
        NVFLIPWHY("copyToScanout: framebuffer would not describe its scanout (0x%x kva %p pitch %u)",
                  (unsigned)ret, q.kva, q.pitch);
        return false;
    }

    const unsigned rows = (h < q.height) ? h : q.height;
    const unsigned rowb = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    const unsigned words = rowb / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    const volatile unsigned int *s = (const volatile unsigned int *)vm->fKva;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
    for (unsigned y = 0; y < rows; y++) {
        const volatile unsigned int *sp = s + (unsigned long)y * spr;
        volatile unsigned int *dp = d + (unsigned long)y * dpr;
        for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
    }
    nvScanoutCopies++;
    if (nvScanoutCopies <= 3 || nvScanoutCopies == NVSCANOUT_COPY_MAX)
        ALOG("*** COPIED THE COMPOSITED FRAME ONTO THE PANEL *** #%u: %ux%u, %u rows x %u bytes, "
             "src kva %p pitch %u -> scanout kva %p phys 0x%llx pitch %u%s",
             nvScanoutCopies, w, h, rows, rowb, vm->fKva, pitch, q.kva,
             (unsigned long long)q.phys, q.pitch,
             nvScanoutCopies == NVSCANOUT_COPY_MAX ? "  [CAP REACHED -- stopping, panel keeps this frame]" : "");
    return true;
}

static bool nvAccelFlipTo(IOAccelResource2 *res)
{
    if (!res) { NVFLIPWHY("nvAccelFlipTo: no resource"); return false; }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("nvAccelFlipTo: accel %p fFB %p", a, a ? a->fFB : nullptr); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("nvAccelFlipTo: refused -- heads piped 0x%x, no way to tell whose surface this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fRmHandle) {
        NVFLIPWHY("nvAccelFlipTo: vm %p fRmHandle %p -- the composited surface has no RM memory to flip to",
                  vm, vm ? vm->fRmHandle : nullptr);
        return false;
    }
    if (vm->fScanout) { NVFLIPWHY("nvAccelFlipTo: already the scanout"); return false; }

    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || !pitch) { NVFLIPWHY("nvAccelFlipTo: geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMFlipRequest f;
    volatile unsigned char *z = (volatile unsigned char *)&f;
    for (unsigned i = 0; i < sizeof f; i++) z[i] = 0;
    f.version = NVRM_FLIP_ABI_VERSION;
    f.kapiMemory = vm->fRmHandle; f.allocationCookie = vm->fAllocationCookie;
    f.width = w; f.height = h; f.pitch = pitch;

    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_FLIP_FN);
    if (!sym) { NVFLIPWHY("nvAccelFlipTo: OSSymbol allocation failed"); return false; }
    IOReturn rr = a->fFB->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr);
    sym->release();
    if (rr != kIOReturnSuccess) {
        NVFLIPWHY("nvAccelFlipTo: framebuffer refused the flip (0x%x, flipResult %d) for %ux%u pitch %u kapiMemory %p",
                  (unsigned)rr, f.flipResult, w, h, pitch, vm->fRmHandle);
        return false;
    }
    NVFLIPWHY("nvAccelFlipTo: *** FLIPPED *** to %ux%u pitch %u kapiMemory %p (flipResult %d)",
              w, h, pitch, vm->fRmHandle, f.flipResult);
    nvAccelSampleFront(vm, w, h, pitch);
    return true;
}

IOReturn NVAccel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && p1 && fn->isEqualTo(NVACCEL_SS_FN))
        return nvAccelSurfVram((struct NVRMSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

NM_TAHOE_FWD_DEFS
